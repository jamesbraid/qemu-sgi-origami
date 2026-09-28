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
from qemu_test.asset import LocalAsset

from lib_graphics_matrix import disk_rel
from lib_prom import FIRMWARE_REL
from test_graphics_matrix import register_graphics_matrix_cells


class GraphicsMatrixThorough(QemuSystemTest):
    """One test method per thorough row of graphics-matrix."""

    ASSET_PROM = LocalAsset(
        FIRMWARE_REL,
        'c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd')
    ASSET_DISK_ROOT_RAD4 = LocalAsset(
        disk_rel('root-rad4'),
        'f47c806f6a1234044668bbd67fd820a3f9e7dff80ff97edc4b84be8ba84887bc')


register_graphics_matrix_cells(GraphicsMatrixThorough, 'thorough')


if __name__ == '__main__':
    QemuSystemTest.main()
