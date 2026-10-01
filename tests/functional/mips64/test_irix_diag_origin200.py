#!/usr/bin/env python3
#
# IRIX online diagnostics and IOC3 port checks on the one-chassis Origin 200
#
# SPDX-License-Identifier: BSD-3-Clause
"""
The origin200 row of irix-diag-matrix, in its own meson target so the
nightly runs it beside the other flavours. test_irix_diag.py owns the
boot and the checks.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_irix_diag import (QemuSystemTest, IrixDiagMatrix,  # noqa: E402
                            DIAG_MEDIUM_SPEC, IP27_DIAG_DISK_SPEC,
                            IP27_FIRMWARE_SPEC,
                            register_irix_diag_row)
from qemu_test.asset import LocalAsset


class IrixDiagOrigin200(IrixDiagMatrix):
    """the one-chassis Origin 200"""

    ASSET_FIRMWARE = LocalAsset(*IP27_FIRMWARE_SPEC)
    ASSET_DISK = LocalAsset(*IP27_DIAG_DISK_SPEC)
    ASSET_MEDIUM = LocalAsset(*DIAG_MEDIUM_SPEC)


register_irix_diag_row(IrixDiagOrigin200, 'origin200')


if __name__ == '__main__':
    QemuSystemTest.main()
