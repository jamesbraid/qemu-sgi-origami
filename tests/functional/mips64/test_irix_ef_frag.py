#!/usr/bin/env python3
#
# Bidirectional fragmented UDP on an IRIX ef adapter.
#
# SPDX-License-Identifier: BSD-3-Clause
"""Require ef0 to reassemble a peer datagram and transmit one UDP datagram."""

from qemu_test import QemuSystemTest, skipBigDataTest, skipIfMissingEnv
from qemu_test import skipSlowTest
from qemu_test.asset import LocalAsset

import test_irix_eg_frag as base

EF_MAC = bytes.fromhex("0800690ec601")


class IrixEfFrag(base.IrixEgFrag):
    ASSET_FIRMWARE = LocalAsset(base.FIRMWARE_REL, base.PROM_SHA256)
    ASSET_DISK = LocalAsset(base.DISK_REL, base.DISK_SHA256)

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv("QEMU_TEST_SGI_ASSET_DIR")
    def test_bidirectional_fragmented_udp(self):
        base.run_bidirectional_fragmented_udp(
            self, "ef", None, EF_MAC, guest_ifname="ef0")


if __name__ == "__main__":
    QemuSystemTest.main()
