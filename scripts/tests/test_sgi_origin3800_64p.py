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

from sgi_model.model_qemu import render_qemu_inputs
from sgi_model.model_validation import load_checked_system_model, schema

# The cabling draft's 64-processor table (Table 2-14): router index is
# rack-major, so 0 and 1 are rack 001's unit-19 and unit-27 routers and 2
# and 3 are rack 002's.
ROUTER_LINKS = {
    ((0, 1), (1, 1)), ((0, 6), (1, 6)), ((2, 1), (3, 1)), ((2, 6), (3, 6)),
    ((0, 7), (2, 7)), ((0, 8), (3, 8)), ((1, 7), (3, 7)), ((1, 8), (2, 8)),
}
BAYS = [10, 13, 16, 21, 24, 29, 32, 35]
ONE_RACK = ("origin3800", "origin3800-high-bandwidth")


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


class Origin380064pTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = load_checked_system_model(
            INPUTS / "origin3800-64p.yaml", model_schema=schema(INPUTS))
        cls.catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sgi-machines.json"])
        [cls.topology] = [t for t in cls.catalogue["topologies"]
                          if t["name"] == "origin3800-64p"]

    def test_three_racks_are_numbered(self):
        racks = {entry["component"]: entry["number"] for entry in
                 self.model["machine"]["emulation"]["rack_numbers"]}
        self.assertEqual(racks,
                         {"rack-001": 1, "rack-002": 2, "rack-101": 101})

    def test_bricks_sit_at_the_drawn_racks_and_bays(self):
        placed = {m["id"]: (m["rack"], m["initial-module-number"])
                  for m in self.topology["modules"]}
        expected = {}
        for c in range(16):
            expected[f"c{c:02d}-chassis"] = (1 + c // 8, BAYS[c % 8])
        for r in range(4):
            expected[f"rbrick{r}-brick"] = (1 + r // 2, 19 if r % 2 == 0
                                            else 27)
        # The I-brick of the I/O rack: units 20 to 23.
        expected["ibrick-brick"] = (101, 20)
        self.assertEqual(placed, expected)
        # A rack and bay name one brick.
        self.assertEqual(len(set(placed.values())), len(placed))

    def test_c_bricks_are_nodes_in_rack_order(self):
        by_node = {m["nodes"][0]: m["id"] for m in self.topology["modules"]
                   if m.get("brick-type") == "C"}
        self.assertEqual(by_node,
                         {n: f"c{n:02d}-chassis" for n in range(16)})

    def test_cables_are_the_draft_table(self):
        links, c_bricks = set(), {}
        ports = []
        for cable in self.topology["cables"]:
            ends = [(e["kind"], e["index"], e["port"]) for e in cable["ends"]]
            ports += [e for e in ends if e[0] == "router"]
            if ends[0][0] == "router":
                links.add(tuple((i, p) for _, i, p in ends))
            else:
                node, router = ends
                c_bricks[node[1]] = (router[1], router[2])
        self.assertEqual(links, ROUTER_LINKS)
        # Four C-bricks on ports 2 to 5 of each router, in node order.
        self.assertEqual(c_bricks, {n: (n // 4, 2 + n % 4)
                                    for n in range(16)})
        self.assertEqual(len(ports), len(set(ports)))
        self.assertEqual(len(self.topology["cables"]), 24)
        self.assertEqual(len(self.topology["routers"]), 4)

    def test_i_brick_is_cabled_to_the_first_c_brick(self):
        # The labelled model choice: which C-brick an I/O rack's cable
        # reaches is unknown.
        # It takes the I-brick's XIO 10 (established for the I-brick).
        [cable] = self.topology["xio-cables"]
        self.assertEqual(cable["ends"], [
            {"kind": "node", "index": 0},
            {"kind": "crossbar", "index": 0, "port": 10}])

    def test_offering_defaults_to_16_gib(self):
        [offering] = [o for o in self.catalogue["offerings"]
                      if o["topology"] == "origin3800-64p"]
        self.assertEqual(offering["nodes"], 16)
        self.assertEqual(offering["smp"], 64)
        self.assertEqual(offering["memory-per-node-mib"],
                         {"default": 1024, "accepted": [1024, 4096]})
        self.assertEqual(offering["machine-options"],
                         "origin3000,topology=origin3800-64p,nodes=16")

    def test_one_rack_topologies_keep_4096_only(self):
        for name in ONE_RACK:
            [offering] = [o for o in self.catalogue["offerings"]
                          if o["topology"] == name]
            self.assertEqual(offering["memory-per-node-mib"],
                             {"default": 4096, "accepted": [4096]}, name)
            [topology] = [t for t in self.catalogue["topologies"]
                          if t["name"] == name]
            for module in topology["modules"]:
                self.assertNotIn("rack", module)


if __name__ == "__main__":
    unittest.main()
