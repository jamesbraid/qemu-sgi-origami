# SPDX-License-Identifier: BSD-3-Clause

"""Tests for the standalone SGI machine-model generator."""

import copy
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
INPUTS = ROOT / "hw/mips/sgi/models"
EXPECTED_CATALOGUE_SHA256 = "8043bfb395b001443f62184bb80d514fc119dabae097671b57272aebb8fd34fc"

sys.path.insert(0, str(ROOT / "scripts"))

from sgi_model import model_composition
from sgi_model.exceptions import ResolutionError
from sgi_model.model_validation import load_checked_system_model, schema
from sgi_model.model_qemu import generate_qemu_inputs, render_qemu_inputs
from sgi_model.model_validation import discover_model_paths, load_checked_system_models
from sgi_model.system_model import _layer
from sgi_model.registry import IMPLEMENTED_TOPOLOGIES, PRODUCT_NAMES


class GeneratorTests(unittest.TestCase):
    def test_default_export_is_complete_and_compatible(self):
        outputs = render_qemu_inputs(INPUTS)
        catalogue_text = outputs["hw/mips/sgi/sn-catalogue.json"]
        catalogue = json.loads(catalogue_text)
        manifest = json.loads(outputs["hw/mips/sgi/sn-model.json"])
        self.assertEqual(
            {item["name"] for item in catalogue["topologies"]},
            set(IMPLEMENTED_TOPOLOGIES))
        self.assertEqual(
            {item["product"] for item in catalogue["offerings"]},
            set(PRODUCT_NAMES))
        self.assertEqual(
            {item["topology"] for item in catalogue["offerings"]},
            set(IMPLEMENTED_TOPOLOGIES))
        expected_offerings = []
        for topology in catalogue["topologies"]:
            for row in topology["node-populations"]:
                if row["grade"] == "unknown":
                    continue
                for population in row["supported-cpu-populations"]:
                    expected_offerings.append((
                        topology["product"], topology["name"],
                        len(row["nodes"]), tuple(population["cpus-per-node"])))
        actual_offerings = [(
            item["product"], item["topology"], item["nodes"],
            tuple(item["cpus-per-node"])) for item in catalogue["offerings"]]
        self.assertEqual(sorted(actual_offerings), sorted(expected_offerings))
        self.assertEqual(manifest["format"], 4)
        self.assertEqual(manifest["format_key"], "sgi-sn")
        self.assertEqual(
            hashlib.sha256(catalogue_text.encode()).hexdigest(),
            EXPECTED_CATALOGUE_SHA256)
        self.assertEqual(
            manifest["outputs"]["hw/mips/sgi/sn-catalogue.json"],
            EXPECTED_CATALOGUE_SHA256)
        checked = load_checked_system_models(
            discover_model_paths(INPUTS), model_root=INPUTS)
        resolved = {
            path.stem: model for path, model in checked
            if path.stem in IMPLEMENTED_TOPOLOGIES
            and model["machine"]["model_role"] == "emulation-target"
            and model["machine"]["emulation"]["lifecycle"] == "implemented"
            and model["machine"]["emulation"]["machine_name"] in PRODUCT_NAMES
        }
        expected_source_digest = hashlib.sha256(json.dumps(
            resolved, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        self.assertEqual(manifest["source_sha256"], expected_source_digest)
        self.assertEqual(render_qemu_inputs(INPUTS), outputs)
        self.assertEqual(set(manifest["outputs"]),
                         {"hw/mips/sgi/sn-catalogue.json"})

    def test_cli_rejects_an_incompatible_tree_before_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            tree = pathlib.Path(temp)
            result = subprocess.run(
                [sys.executable, str(ROOT / "scripts/sgi-model.py"),
                 "--inputs", str(INPUTS), "--tree", str(tree)],
                cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cannot read QEMU model format declarations", result.stderr)
            self.assertFalse((tree / "hw/mips/sgi/sn-catalogue.json").exists())
            self.assertFalse((tree / "hw/mips/sgi/sn-model.json").exists())

    def test_cli_rejects_a_wrong_format_before_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            tree = pathlib.Path(temp)
            contract = tree / "hw/mips/origami-model-formats.json"
            contract.parent.mkdir(parents=True)
            contract.write_text('{"sgi-sn": 3}\n')
            result = subprocess.run(
                [sys.executable, str(ROOT / "scripts/sgi-model.py"),
                 "--inputs", str(INPUTS), "--tree", str(tree)],
                cwd=ROOT, text=True, capture_output=True, check=False,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("does not accept the sgi-sn model at format 4",
                          result.stderr)
            self.assertFalse((tree / "hw/mips/sgi/sn-catalogue.json").exists())
            self.assertFalse((tree / "hw/mips/sgi/sn-model.json").exists())

    def test_cli_installs_into_a_format_four_tree(self):
        with tempfile.TemporaryDirectory() as temp:
            tree = pathlib.Path(temp)
            contract = tree / "hw/mips/origami-model-formats.json"
            contract.parent.mkdir(parents=True)
            contract.write_text('{"sgi-sn": 4}\n')
            result = subprocess.run(
                [sys.executable, str(ROOT / "scripts/sgi-model.py"),
                 "--inputs", str(INPUTS), "--tree", str(tree)],
                cwd=ROOT, text=True, capture_output=True, check=False,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
            self.assertEqual(result.returncode, 0, result.stderr)
            expected = render_qemu_inputs(INPUTS)
            for path, contents in expected.items():
                self.assertEqual((tree / path).read_text(), contents)
            self.assertEqual(
                set(result.stdout.splitlines()),
                {str(tree / path) for path in expected})

    def test_every_authored_model_layer_has_supported_structure(self):
        layers = sorted((INPUTS / "layers").rglob("*.yaml"))
        self.assertEqual(len(list(INPUTS.rglob("*.yaml"))), 92)
        self.assertEqual(len(discover_model_paths(INPUTS)), 28)
        self.assertEqual(len(layers), 64)
        for path in layers:
            with self.subTest(path=path.relative_to(INPUTS)):
                self.assertIn(_layer(path)["kind"], {
                    "architecture", "assembly", "module", "product"})

    def test_cli_output_exports_without_a_consumer_tree(self):
        with tempfile.TemporaryDirectory() as temp:
            output = pathlib.Path(temp)
            result = subprocess.run(
                [sys.executable, str(ROOT / "scripts/sgi-model.py"),
                 "--inputs", str(INPUTS), "--output", str(output)],
                cwd=ROOT, text=True, capture_output=True, check=False,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
            self.assertEqual(result.returncode, 0, result.stderr)
            for path, contents in render_qemu_inputs(INPUTS).items():
                self.assertEqual((output / path).read_text(), contents)

    def test_generation_uses_explicit_inputs_and_writes_only_catalogue_files(self):
        with tempfile.TemporaryDirectory() as temp:
            tree = pathlib.Path(temp)
            paths = generate_qemu_inputs(INPUTS, tree)
            self.assertEqual(
                {str(path.relative_to(tree)) for path in paths},
                {"hw/mips/sgi/sn-catalogue.json", "hw/mips/sgi/sn-model.json"})
            files = {str(path.relative_to(tree)) for path in tree.rglob("*")
                     if path.is_file()}
            self.assertEqual(files, {
                "hw/mips/sgi/sn-catalogue.json", "hw/mips/sgi/sn-model.json"})
            self.assertFalse((tree / "docs/datasheets").exists())
            self.assertFalse((tree / "assets").exists())


class FuelTests(unittest.TestCase):
    def test_fuel_offerings_keep_all_console_ports(self):
        catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sn-catalogue.json"])
        offerings = [row for row in catalogue["offerings"]
                     if row["product"] == "fuel"]
        self.assertEqual({row["topology"] for row in offerings},
                         {"fuel"})
        expected = [
            {"kind": "system-controller", "node": 0,
             "argument": "-serial chardev:l1"},
            {"kind": "second-port", "node": 0,
             "argument": "-serial chardev:ioc3_b"},
            {"kind": "console", "node": 0,
             "argument": "-serial chardev:ioc3_a"},
        ]
        for row in offerings:
            with self.subTest(topology=row["topology"]):
                self.assertEqual(row["console"], "ioc3-serial-a")
                self.assertEqual(row["consoles"], expected)

    def test_fuel_export_has_system_timekeeper(self):
        catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sn-catalogue.json"])
        topologies = {row["name"]: row for row in catalogue["topologies"]}
        self.assertIn("fuel", topologies)
        self.assertNotIn("fuel-m48t35-compatible", topologies)
        fitted = topologies["fuel"]["io-boards"]
        self.assertEqual(len(fitted), 1)
        self.assertEqual(fitted[0]["preset"], "fuel")
        self.assertEqual(fitted[0]["system-timekeeper"], {
            "kind": "m48t35-compatible", "year-base": 1968,
            "initial-ip27-fixture": False})
        for row in catalogue["offerings"]:
            if row["product"] != "fuel":
                continue
            self.assertEqual(row["default-topology"], "fuel")
            resources = [item for item in row["resources"]
                         if item["kind"] == "nvram"]
            self.assertEqual(len(resources), 1)
            resource = resources[0]
            self.assertEqual(resource["board-indices"], [0])
            self.assertEqual(resource["drive-indices"], [0])
            self.assertEqual(resource["part"], "m48t35")
            self.assertEqual(resource["size"], 32768)
            self.assertIs(resource["clock-reference"], True)
            self.assertEqual(resource["year-base"], 1968)
            self.assertIs(resource["initial-ip27-fixture"], False)
            self.assertEqual(resource["system-timekeeper-kind"],
                             "m48t35-compatible")
            self.assertNotIn("requires-machine-option", resource)
            self.assertNotIn("conflicts-machine-option", resource)

    def test_system_timekeeper_requires_fitted_fuel_board(self):
        model = load_checked_system_model(
            INPUTS / "fuel.yaml", model_schema=schema(INPUTS))
        for field, value in (("generation", "SN0"), ("board_ip", "IP29")):
            altered = copy.deepcopy(model)
            altered["machine"][field] = value
            with self.subTest(field=field), self.assertRaises(ResolutionError):
                model_composition.validate_system_timekeepers(altered)
        altered = copy.deepcopy(model)
        altered["machine"]["emulation"]["system_timekeepers"]["absent"] = (
            altered["machine"]["emulation"]["system_timekeepers"].pop(
                "ip34-system-board"))
        with self.assertRaises(ResolutionError):
            model_composition.validate_system_timekeepers(altered)

    def test_system_timekeeper_rejects_unknown_clock_policy(self):
        model = load_checked_system_model(
            INPUTS / "fuel.yaml", model_schema=schema(INPUTS))
        for field, value in (("kind", "ds1742"), ("year_base", 1970),
                             ("year_base", True),
                             ("initial_ip27_fixture", True)):
            altered = copy.deepcopy(model)
            altered["machine"]["emulation"]["system_timekeepers"][
                "ip34-system-board"][field] = value
            with self.subTest(field=field), self.assertRaises(ResolutionError):
                model_composition.validate_system_timekeepers(altered)



if __name__ == "__main__":
    unittest.main()
