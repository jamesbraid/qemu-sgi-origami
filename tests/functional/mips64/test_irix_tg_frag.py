#!/usr/bin/env python3
#
# Bidirectional fragmented UDP on an IRIX tg adapter.
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require tg1 to reassemble a peer datagram and transmit one UDP datagram."""

from qemu_test import QemuSystemTest, skipBigDataTest, skipIfMissingEnv
from qemu_test import skipSlowTest
from qemu_test.asset import LocalAsset

from lib_prom import FIRMWARE_REL
import test_irix_eg_frag as base

TG_MAC = bytes.fromhex("525400bc5701")


class IrixTgFrag(base.IrixEgFrag):
    ASSET_FIRMWARE = LocalAsset(FIRMWARE_REL, base.PROM_SHA256)
    ASSET_DISK = LocalAsset(base.DISK_REL, base.DISK_SHA256)

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv("QEMU_TEST_SGI_ASSET_DIR")
    def test_bidirectional_fragmented_udp(self):
        base.run_bidirectional_fragmented_udp(self, "tg", "bcm570x", TG_MAC)


if __name__ == "__main__":
    QemuSystemTest.main()
