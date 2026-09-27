#!/usr/bin/env python3
#
# IRIX online diagnostics and IOC3 port checks on a four-processor
# Origin 300
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The origin300_4cpu row of irix-diag-matrix, in its own meson
target so the nightly runs it beside the other flavours.
test_irix_diag.py owns the boot and the checks.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_irix_diag import (QemuSystemTest, IrixDiagMatrix,  # noqa: E402
                            register_irix_diag_row)


class IrixDiagOrigin300FourCpus(IrixDiagMatrix):
    """the Origin 300 with four processors"""


register_irix_diag_row(IrixDiagOrigin300FourCpus, 'origin300_4cpu')


if __name__ == '__main__':
    QemuSystemTest.main()
