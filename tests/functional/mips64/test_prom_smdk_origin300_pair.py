#!/usr/bin/env python3
#
# The sMDK standalone diagnostics on the two-chassis Origin 300
#
# SPDX-License-Identifier: BSD-3-Clause
"""
The origin300_pair row of smdk-matrix, in its own meson target so the
nightly runs it beside the other populations. test_prom_smdk_origin300.py
owns the boots and the verdicts.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_prom_smdk_origin300 import (QemuSystemTest,  # noqa: E402
                                      PromSmdkMatrix, register_smdk_row)


class PromSmdkOrigin300Pair(PromSmdkMatrix):
    """Two Origin 300 chassis joined by NUMAlink, four processors each."""


register_smdk_row(PromSmdkOrigin300Pair, 'origin300_pair')


if __name__ == '__main__':
    QemuSystemTest.main()
