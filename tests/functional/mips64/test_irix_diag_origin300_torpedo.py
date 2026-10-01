#!/usr/bin/env python3
#
# IRIX's torpedo online diagnostic on the two-processor Origin 300
#
# SPDX-License-Identifier: BSD-3-Clause
"""
The origin300_torpedo row of irix-diag-matrix, in its own meson
target so the nightly runs it beside the other flavours.
test_irix_diag.py owns the boot and the checks.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_irix_diag import (QemuSystemTest, IrixDiagMatrix,  # noqa: E402
                            register_irix_diag_row)


class IrixDiagOrigin300Torpedo(IrixDiagMatrix):
    """torpedo on the two-processor Origin 300"""


register_irix_diag_row(IrixDiagOrigin300Torpedo, 'origin300_torpedo')


if __name__ == '__main__':
    QemuSystemTest.main()
