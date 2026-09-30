# SPDX-License-Identifier: BSD-3-Clause

"""Tests for public system-model resolution and compilation."""

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
INPUTS = ROOT / "hw/mips/sgi/models"
sys.path.insert(0, str(ROOT / "scripts"))

from sgi_model.model_compile import compile_model
from sgi_model.model_validation import discover_model_paths, load_checked_system_models
from sgi_model.registry import IMPLEMENTED_TOPOLOGIES


class ResolutionTests(unittest.TestCase):
    def test_every_implemented_input_resolves_and_compiles(self):
        checked = load_checked_system_models(
            discover_model_paths(INPUTS), model_root=INPUTS)
        compiled = {
            compile_model(model)["name"]: model
            for path, model in checked if path.stem in IMPLEMENTED_TOPOLOGIES
        }
        self.assertEqual(set(compiled), set(IMPLEMENTED_TOPOLOGIES))
        for name, model in compiled.items():
            self.assertEqual(model["machine"]["model_role"], "emulation-target")
            self.assertEqual(model["machine"]["emulation"]["lifecycle"], "implemented")
            self.assertTrue(model["components"])


if __name__ == "__main__":
    unittest.main()
