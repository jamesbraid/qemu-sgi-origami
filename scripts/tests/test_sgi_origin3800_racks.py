# SPDX-License-Identifier: BSD-3-Clause

"""Tests for the multi-rack Origin 3800 topologies above 64 processors.

The cable tables are written out here from the cabling draft's tables, not
read back from the topology plans, so a plan edit cannot move both.
"""

import json
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
INPUTS = ROOT / "hw/mips/sgi/models"
sys.path.insert(0, str(ROOT / "scripts"))

from sgi_model.model_qemu import render_qemu_inputs
from sgi_model.model_validation import load_checked_system_model, schema

BAYS = [10, 13, 16, 21, 24, 29, 32, 35]

# Table 2-18. Router index is rack-major: 2n is rack n's unit-19 router and
# 2n + 1 its unit-27 router, racks in the order 001 to 004.
LINKS_128 = (
    {((2 * r, 1), (2 * r + 1, 1)) for r in range(4)} |
    {((2 * r, 6), (2 * r + 1, 6)) for r in range(4)} |
    {((0, 7), (2, 7)), ((1, 7), (3, 7)), ((4, 7), (6, 7)), ((5, 7), (7, 7)),
     ((0, 8), (4, 8)), ((1, 8), (5, 8)), ((2, 8), (6, 8)), ((3, 8), (7, 8))})


def _topology(name):
    catalogue = json.loads(render_qemu_inputs(INPUTS)[
        "hw/mips/sgi/sgi-machines.json"])
    [topology] = [t for t in catalogue["topologies"] if t["name"] == name]
    [offering] = [o for o in catalogue["offerings"] if o["topology"] == name]
    return topology, offering


class MultiRackOrigin3800(unittest.TestCase):
    NAME = RACKS = LINKS = None

    @classmethod
    def setUpClass(cls):
        if cls.NAME is None:
            raise unittest.SkipTest("base class")
        cls.model = load_checked_system_model(
            INPUTS / f"{cls.NAME}.yaml", model_schema=schema(INPUTS))
        cls.topology, cls.offering = _topology(cls.NAME)
        cls.nodes = 8 * len(cls.RACKS)

    def test_racks_are_numbered_with_the_io_rack(self):
        racks = {e["component"]: e["number"] for e in
                 self.model["machine"]["emulation"]["rack_numbers"]}
        want = {f"rack-{r:03d}": r for r in self.RACKS}
        want["rack-101"] = 101
        self.assertEqual(racks, want)

    def test_c_bricks_are_nodes_in_rack_order_at_the_drawn_bays(self):
        placed = {m["nodes"][0]: (m["rack"], m["initial-module-number"])
                  for m in self.topology["modules"]
                  if m.get("brick-type") == "C"}
        self.assertEqual(placed, {
            n: (self.RACKS[n // 8], BAYS[n % 8]) for n in range(self.nodes)})
        every = [(m["rack"], m["initial-module-number"])
                 for m in self.topology["modules"]]
        self.assertEqual(len(every), len(set(every)))

    def test_cables_are_the_draft_table(self):
        links, c_bricks, ports = set(), {}, []
        for cable in self.topology["cables"]:
            ends = [(e["kind"], e["index"], e["port"]) for e in cable["ends"]]
            ports += [e for e in ends if e[0] == "router"]
            if ends[0][0] == "router":
                links.add(tuple((i, p) for _, i, p in ends))
            else:
                node, router = ends
                c_bricks[node[1]] = (router[1], router[2])
        self.assertEqual(links, self.LINKS)
        # Four C-bricks on ports 2 to 5 of each rack router, in node order.
        self.assertEqual(c_bricks, {n: (n // 4, 2 + n % 4)
                                    for n in range(self.nodes)})
        self.assertEqual(len(ports), len(set(ports)))
        self.assertEqual(len(self.topology["cables"]),
                         self.nodes + len(self.LINKS))

    def test_offering_defaults_to_one_gib_a_node(self):
        self.assertEqual(self.offering["nodes"], self.nodes)
        self.assertEqual(self.offering["smp"], 4 * self.nodes)
        self.assertEqual(self.offering["memory-per-node-mib"],
                         {"default": 1024, "accepted": [1024]})
        self.assertEqual(self.offering["machine-options"],
                         f"origin3000,topology={self.NAME},nodes={self.nodes}")


class Origin3800At128(MultiRackOrigin3800):
    NAME = "origin3800-128p"
    RACKS = (1, 2, 3, 4)
    LINKS = LINKS_128

    def test_eight_routers_and_48_cables_and_no_role(self):
        self.assertEqual(len(self.topology["routers"]), 8)
        self.assertEqual(len(self.topology["cables"]), 48)
        for router in self.topology["routers"]:
            self.assertNotIn("role", router)


if __name__ == "__main__":
    unittest.main()
