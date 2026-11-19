# SPDX-License-Identifier: BSD-3-Clause

"""Tests for the Origin 3800 topologies above 64 processors.

The cable tables are the draft's, not read from the plans.
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

# Table 2-23: routers 16 to 19 are the MetaRouters of racks 002, 003, 012
# and 013; each port maps to the (rack, unit) router whose port 8 reaches it.
RACKS_256 = (1, 2, 3, 4, 11, 12, 13, 14)
UPLINKS_256 = {
    16: {3: (1, 19), 4: (3, 19), 7: (13, 27), 8: (11, 27)},
    17: {3: (2, 19), 4: (4, 19), 7: (14, 27), 8: (12, 27)},
    18: {3: (13, 19), 4: (11, 19), 7: (1, 27), 8: (3, 27)},
    19: {3: (14, 19), 4: (12, 19), 7: (2, 27), 8: (4, 27)},
}


def _router(rack, unit):
    return 2 * RACKS_256.index(rack) + (0 if unit == 19 else 1)


LINKS_256 = (
    {((2 * r, 1), (2 * r + 1, 1)) for r in range(8)} |
    {((2 * r, 6), (2 * r + 1, 6)) for r in range(8)} |
    {((2 * a + u, 7), (2 * a + 2 + u, 7)) for a in (0, 2, 4, 6)
     for u in (0, 1)} |
    {((_router(*where), 8), (meta, port))
     for meta, ports in UPLINKS_256.items()
     for port, where in ports.items()})


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


class Origin3800At256(MultiRackOrigin3800):
    NAME = "origin3800-256p"
    RACKS = RACKS_256
    LINKS = LINKS_256

    def test_twenty_routers_and_104_cables(self):
        self.assertEqual(len(self.topology["routers"]), 20)
        self.assertEqual(len(self.topology["cables"]), 104)

    def test_the_four_metarouters_have_a_role_and_no_c_brick(self):
        roles = {i: r.get("role")
                 for i, r in enumerate(self.topology["routers"])}
        self.assertEqual(roles, {i: 14 if i >= 16 else None
                                 for i in range(20)})
        for cable in self.topology["cables"]:
            for end in cable["ends"]:
                if end["kind"] == "router" and end["index"] >= 16:
                    self.assertEqual(
                        [e["kind"] for e in cable["ends"]],
                        ["router", "router"])

    def test_metarouters_sit_at_unit_38_of_the_four_racks(self):
        placed = {m["id"]: (m["rack"], m["initial-module-number"])
                  for m in self.topology["modules"]}
        self.assertEqual({k: v for k, v in placed.items() if k >= "rbrick16"
                          and k.endswith("-brick")}, {
            "rbrick16-brick": (2, 38), "rbrick17-brick": (3, 38),
            "rbrick18-brick": (12, 38), "rbrick19-brick": (13, 38)})


if __name__ == "__main__":
    unittest.main()
