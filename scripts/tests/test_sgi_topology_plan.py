# SPDX-License-Identifier: BSD-3-Clause

"""Tests that the topology plans reproduce the checked-in Origin 3000 files."""

import contextlib
import copy
import io
import pathlib
import re
import shutil
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
MODELS = ROOT / "hw/mips/sgi/models"
sys.path.insert(0, str(ROOT / "scripts"))

from sgi_model import topology_plan as tp

# Origin 3000 compositions whose text has no per-brick or per-cable run.
# An Origin 3900 of more than four Cx-bricks stays unplanned, not just
# unlisted: no source gives its port-level cabling.
UNPLANNED = {"origin3200", "origin3200-two-ibricks", "origin3200-dbrick",
             "origin3200-pbrick", "origin3200-xbrick", "onyx3000",
             "origin3900", "origin3900-memory-node"}


def _main(models):
    with contextlib.redirect_stdout(io.StringIO()):
        return tp.main(models, False)


def _names():
    return {p.stem for p in MODELS.glob("*.yaml")
            if re.match(r"(origin3[2-9]|onyx3[0-9])00", p.stem)}


class PlanReproducesFiles(unittest.TestCase):
    def test_every_plan_matches_its_files_byte_for_byte(self):
        plans = tp.plans(MODELS)
        self.assertTrue(plans)
        for plan_path in plans:
            for path, text in tp.expand_plan(MODELS, plan_path).items():
                self.assertEqual(path.read_text(), text, path.name)

    def test_every_origin_3000_topology_is_planned_or_listed(self):
        planned = {p.stem for p in tp.plans(MODELS)}
        self.assertEqual(_names() - planned - UNPLANNED, set())
        self.assertFalse(planned & UNPLANNED)

    def test_main_reports_a_stale_file_and_passes_a_clean_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            models = pathlib.Path(tmp) / "models"
            shutil.copytree(MODELS, models)
            self.assertEqual(_main(models), 0)
            path = models / "origin3800.yaml"
            path.write_text(path.read_text().replace(
                "rbrick1-port-6", "rbrick1-port-9", 1))
            self.assertEqual(_main(models), 1)

    def test_main_reports_a_generated_run_with_no_plan(self):
        with tempfile.TemporaryDirectory() as tmp:
            models = pathlib.Path(tmp) / "models"
            shutil.copytree(MODELS, models)
            (models / "plans/onyx3400.yaml").unlink()
            self.assertEqual(_main(models), 1)

    def test_a_block_naming_another_provenance_is_refused(self):
        plan = tp.load_plan(MODELS / "plans/origin3800.yaml")
        plan.data["blocks"]["model-cbrick-links"]["provenance"] = "derived"
        with self.assertRaises(ValueError):
            tp.render_block(plan, "model-cbrick-links")


class OneCableChange(unittest.TestCase):
    def test_moving_one_inter_rack_cable_changes_only_its_runs(self):
        plan_path = MODELS / "plans/origin3800-64p.yaml"
        before = tp.load_plan(plan_path)
        data = copy.deepcopy(before.data)
        link = next(l for l in data["links"] if l["id"] == "router-link-0-3-8")
        link["b"] = "rbrick2"
        after = tp.Plan(data)
        changed = {name for name in data["blocks"]
                   if tp.render_block(before, name) != tp.render_block(after, name)}
        self.assertEqual(changed, {"model-link-ports", "layer-link-buses",
                                   "layer-link-connections"})
        old = tp.render_block(before, "layer-link-connections")
        new = tp.render_block(after, "layer-link-connections")
        self.assertEqual(sum(a != b for a, b in zip(old, new)), 2)


def _small_plan(**router_b):
    return {
        "routers": [{"id": "ra", "label": "R-brick A", "unit": 19},
                    {"id": "rb", "label": "R-brick B", "unit": 38,
                     **router_b}],
        "cbricks": [{"id": "c0", "label": "C-brick 0", "router": "ra",
                     "port": 2, "unit": 10}],
        "links": [{"id": "l", "a": "ra", "b": "rb", "a_port": 8, "b_port": 3}],
        "blocks": {
            "ports": {"kind": "link-ports", "sources": ["s"],
                      "reason": "{own} {port} to {other} {other_port}."},
            "buses": {"kind": "link-buses", "sources": ["s"], "notes": "n"},
            "rows": {"kind": "link-connections", "sources": ["s"]},
            "roles": {"kind": "router-roles",
                      "reasons": {14: "{r} at unit {unit}."}},
            "instances": {"kind": "instances", "model": "m.yaml",
                          "prefixes": ["ra", "rb"], "parent": "rack",
                          "omit_buses": ["p1", "p6"]},
            "units": {"kind": "chassis-numbers", "of": "routers",
                      "suffix": "-brick", "reason": "unit {unit}"},
        },
    }


class UnequalPortsAndRoles(unittest.TestCase):
    def test_a_link_names_the_port_at_each_end(self):
        plan = tp.Plan(_small_plan(role=14))
        plan.check()
        self.assertEqual(
            tp.render_block(plan, "ports")[0], "- component: ra-port-8")
        self.assertIn("  peer: rb-port-3", tp.render_block(plan, "ports"))
        self.assertIn("  name: NUMAlink 3 cable, R-brick A port 8 to "
                      "R-brick B port 3", tp.render_block(plan, "buses"))
        rows = "\n".join(tp.render_block(plan, "rows"))
        for end in ("ra-router, bus: l, role: master, fabric_port: 8",
                    "rb-router, bus: l, role: endpoint, fabric_port: 3",
                    "ra-port-8, bus: l, role: endpoint, fabric_port: 8",
                    "rb-port-3, bus: l, role: endpoint, fabric_port: 3"):
            self.assertIn(end, rows)

    def test_roles_and_router_units_are_generated(self):
        plan = tp.Plan(_small_plan(role=14))
        self.assertEqual(tp.render_block(plan, "roles"), [
            '- {component: rb-router, role: 14, '
            'reason: "R-brick B at unit 38."}'])
        self.assertEqual(
            tp.render_block(plan, "units")[1],
            '- {component: rb-brick, number: 38, reason: "unit 38"}')

    def test_a_router_may_omit_its_own_buses(self):
        data = _small_plan(role=14)
        data["routers"][1]["omit_buses"] = ["p3", "p8"]
        lines = tp.render_block(tp.Plan(data), "instances")
        self.assertEqual([l for l in lines if "omit_buses" in l],
                         ["  omit_buses: [p1, p6]", "  omit_buses: [p3, p8]"])

    def test_a_port_on_two_cables_is_refused(self):
        data = _small_plan()
        data["links"][0]["a_port"] = 2
        with self.assertRaisesRegex(ValueError, "two cables"):
            tp.Plan(data).check()

    def test_a_role_is_12_or_14_and_has_no_c_brick(self):
        for router, role in (("rb", 15), ("ra", 14)):
            data = _small_plan()
            found = next(r for r in data["routers"] if r["id"] == router)
            found["role"] = role
            with self.assertRaises(ValueError):
                tp.Plan(data).check()


if __name__ == "__main__":
    unittest.main()
