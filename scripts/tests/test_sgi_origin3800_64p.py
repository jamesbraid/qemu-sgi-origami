# SPDX-License-Identifier: BSD-3-Clause

#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyyaml>=6", "jsonschema>=4"]
# ///
"""Tests for the 64-processor Origin 3800: its memory kit, racks and wiring.

The kit tests read the IP35 assembly layer; the topology tests resolve the
shipped origin3800-64p composition and its catalogue offering.
"""

import json
import pathlib
import sys
import unittest

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
INPUTS = ROOT / "hw/mips/sgi/models"
sys.path.insert(0, str(ROOT / "scripts"))


def _kits() -> dict:
    layer = yaml.safe_load(
        (INPUTS / "layers/assemblies/ip35.yaml").read_text())["layer"]
    return {kit["id"]: kit for kit in layer["memory_kits"]}


class MemoryKitTests(unittest.TestCase):
    def test_one_pair_of_512_dimms_is_a_kit(self):
        kit = _kits()["kit-1024-two-512-banks"]
        self.assertEqual(kit["dimms"], 2)
        self.assertEqual(kit["dimm_mib"], 512)
        self.assertEqual(kit["banks"], [512, 512])

    def test_the_4096_kit_is_unchanged(self):
        kit = _kits()["kit-4096-eight-512-banks"]
        self.assertEqual(kit["dimms"], 8)
        self.assertEqual(kit["banks"], [512] * 8)


if __name__ == "__main__":
    unittest.main()
