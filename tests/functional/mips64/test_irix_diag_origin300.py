#!/usr/bin/env python3
#
# IRIX online diagnostics and IOC3 port checks on the Origin 300
#
# SPDX-License-Identifier: BSD-3-Clause
"""
The origin300 row of irix-diag-matrix, in its own meson target so the
nightly runs it beside the other flavours. test_irix_diag.py owns the
boot and the checks.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_irix_diag import (QemuSystemTest, IrixDiagMatrix,  # noqa: E402
                            DIAG_MEDIUM_SPEC, IP35_DIAG_DISK_SPEC,
                            IP35_FIRMWARE_SPEC, IP35_SPD_3_SPEC,
                            IP35_SPD_5_SPEC,
                            register_irix_diag_row)
from qemu_test.asset import LocalAsset


class IrixDiagOrigin300(IrixDiagMatrix):
    """the Origin 300"""

    ASSET_FIRMWARE = LocalAsset(*IP35_FIRMWARE_SPEC)
    ASSET_DISK = LocalAsset(*IP35_DIAG_DISK_SPEC)
    ASSET_MEDIUM = LocalAsset(*DIAG_MEDIUM_SPEC)
    ASSET_SPD_3 = LocalAsset(*IP35_SPD_3_SPEC)
    ASSET_SPD_5 = LocalAsset(*IP35_SPD_5_SPEC)


register_irix_diag_row(IrixDiagOrigin300, 'origin300')


if __name__ == '__main__':
    QemuSystemTest.main()
