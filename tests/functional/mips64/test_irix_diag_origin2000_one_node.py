#!/usr/bin/env python3
#
# IRIX online diagnostics and IOC3 port checks on the Origin 2000 one-node
# module's BaseIO-G
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The origin2000_one_node row of irix-diag-matrix, in its own meson target
so the nightly runs it beside the other flavours. test_irix_diag.py owns
the boot and the checks.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_irix_diag import (QemuSystemTest, IrixDiagMatrix,  # noqa: E402
                            register_irix_diag_row)


class IrixDiagOrigin2000OneNode(IrixDiagMatrix):
    """the Origin 2000 one-node module's BaseIO-G"""


register_irix_diag_row(IrixDiagOrigin2000OneNode, 'origin2000_one_node')


if __name__ == '__main__':
    QemuSystemTest.main()
