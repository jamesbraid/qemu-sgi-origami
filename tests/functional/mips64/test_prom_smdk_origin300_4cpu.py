#!/usr/bin/env python3
#
# The sMDK standalone diagnostics on a four-processor Origin 300
#
# SPDX-License-Identifier: BSD-3-Clause
"""
The origin300_4cpu row of smdk-matrix, in its own meson target so the
nightly runs it beside the other populations. test_prom_smdk_origin300.py
owns the boots and the verdicts.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_prom_smdk_origin300 import (QemuSystemTest,  # noqa: E402
                                      PromSmdkMatrix, register_smdk_row)


class PromSmdkOrigin300FourCpus(PromSmdkMatrix):
    """The four-processor Origin 300."""


register_smdk_row(PromSmdkOrigin300FourCpus, 'origin300_4cpu')


if __name__ == '__main__':
    QemuSystemTest.main()
