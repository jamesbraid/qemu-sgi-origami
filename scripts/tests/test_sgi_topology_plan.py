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


if __name__ == "__main__":
    unittest.main()
