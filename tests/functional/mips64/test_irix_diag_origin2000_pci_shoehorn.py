#!/usr/bin/env python3
#
# IRIX online diagnostics and IOC3 port checks on the Origin 2000 four-node
# module with a PCI shoehorn
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The origin2000_pci_shoehorn row of irix-diag-matrix, in its own meson
target so the nightly runs it beside the other flavours.
test_irix_diag.py owns the boot and the checks.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_irix_diag import (QemuSystemTest, IrixDiagMatrix,  # noqa: E402
                            register_irix_diag_row)


class IrixDiagOrigin2000PciShoehorn(IrixDiagMatrix):
    """the Origin 2000 four-node module with a PCI shoehorn"""


register_irix_diag_row(IrixDiagOrigin2000PciShoehorn,
                       'origin2000_pci_shoehorn')


if __name__ == '__main__':
    QemuSystemTest.main()
