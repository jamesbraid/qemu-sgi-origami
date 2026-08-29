#!/usr/bin/env python3
#
# The Origin 200 test matrix, thorough rows: the real-firmware boots
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
One test method per row of origin200-matrix whose speed column says
thorough, run from meson's thorough list. test_origin200_matrix.py owns
the per-row test body and runs the quick rows; this script only selects
the other half of the manifest, so the manifest's speed column maps onto
meson's own quick/thorough split.
"""

from qemu_test import QemuSystemTest

from test_origin200_matrix import register_matrix_cells


class OriginMatrixThorough(QemuSystemTest):
    """One test method per thorough row of origin200-matrix."""


register_matrix_cells(OriginMatrixThorough, 'thorough')


if __name__ == '__main__':
    QemuSystemTest.main()
