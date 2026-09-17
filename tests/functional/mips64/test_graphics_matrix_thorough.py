#!/usr/bin/env python3
#
# The graphics device test matrix, thorough rows: the real-disk boots
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
One test method per row of graphics-matrix whose speed column says
thorough, run from meson's thorough list. test_graphics_matrix.py owns
the per-row test body and runs the quick rows; this script only selects
the other half of the manifest, mirroring
test_origin200_matrix_thorough.py's own relationship to
test_origin200_matrix.py exactly.
"""

from qemu_test import QemuSystemTest

from test_graphics_matrix import register_graphics_matrix_cells


class GraphicsMatrixThorough(QemuSystemTest):
    """One test method per thorough row of graphics-matrix."""


register_graphics_matrix_cells(GraphicsMatrixThorough, 'thorough')


if __name__ == '__main__':
    QemuSystemTest.main()
