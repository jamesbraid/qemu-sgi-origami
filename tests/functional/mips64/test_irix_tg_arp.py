#!/usr/bin/env python3
#
# Peer-originated ARP reply on an IRIX tg adapter.
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot installed IRIX and require tg1 to answer one peer ARP request."""

from qemu_test import QemuSystemTest, skipBigDataTest, skipIfMissingEnv
from qemu_test import skipSlowTest
from qemu_test.asset import LocalAsset

import test_irix_eg_arp as base

TG_MAC = bytes.fromhex("525400bc5701")


class IrixTgArp(base.IrixEgArp):
    ASSET_FIRMWARE = LocalAsset(base.FIRMWARE_REL, base.PROM_SHA256)
    ASSET_DISK = LocalAsset(base.DISK_REL, base.DISK_SHA256)

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv("QEMU_TEST_SGI_ASSET_DIR")
    def test_peer_arp_request_gets_guest_reply(self):
        base.run_peer_arp_request_gets_guest_reply(
            self, "tg", "bcm570x", TG_MAC)


if __name__ == "__main__":
    QemuSystemTest.main()
