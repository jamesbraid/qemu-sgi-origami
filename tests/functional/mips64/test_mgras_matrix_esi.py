#!/usr/bin/env python3
#
# ESI board on the four-node Origin 2000: IRIX desktop and drawing
# features
#
# SPDX-License-Identifier: BSD-3-Clause
"""
Boots IRIX 6.5.30 to the 4Dwm desktop on the Origin 2000 with four nodes,
the BaseIO-G and a sgi-mgras board=esi in IO3, checks hinv's graphics
line, and runs the desktop, lit 3D and pixel transfer against
colour-statistics references. lib_mgras_boot.run_board owns the sequence.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import test_mgras_desktop as desktop  # noqa: E402
from lib_mgras_boot import (DISK_REL, DISK_SHA256,  # noqa: E402
                            run_board)
from qemu_test import QemuSystemTest  # noqa: E402
from qemu_test import skipSlowTest, skipBigDataTest  # noqa: E402
from qemu_test import skipIfMissingEnv  # noqa: E402
from qemu_test.asset import LocalAsset  # noqa: E402

BOARD = 'esi'


class MgrasMatrixEsi(QemuSystemTest):
    """the ESI board's desktop and drawing features"""

    ASSET_FIRMWARE = desktop.MgrasDesktop.ASSET_FIRMWARE
    ASSET_DISK = LocalAsset(DISK_REL, DISK_SHA256)

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_mgras_matrix_esi(self):
        run_board(self, BOARD, self.ASSET_FIRMWARE.fetch(),
                  self.ASSET_DISK.fetch())


if __name__ == '__main__':
    QemuSystemTest.main()
