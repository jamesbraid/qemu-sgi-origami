#!/usr/bin/env python3
#
# IRIX online diagnostics and IOC3 port checks on the two-chassis
# Origin 300
#
# SPDX-License-Identifier: BSD-3-Clause
"""
The origin300_pair row of irix-diag-matrix, in its own meson
target so the nightly runs it beside the other flavours.
test_irix_diag.py owns the boot and the checks.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_irix_diag import (QemuSystemTest, IrixDiagMatrix,  # noqa: E402
                            register_irix_diag_row)


class IrixDiagOrigin300Pair(IrixDiagMatrix):
    """two Origin 300 chassis joined by NUMAlink"""


register_irix_diag_row(IrixDiagOrigin300Pair, 'origin300_pair')


if __name__ == '__main__':
    QemuSystemTest.main()
