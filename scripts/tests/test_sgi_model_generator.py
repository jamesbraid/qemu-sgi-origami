# SPDX-License-Identifier: BSD-3-Clause

"""Tests for the standalone SGI machine-model generator."""

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
EXPECTED_CATALOGUE_SHA256 = "706ff905d96db56ef30db39816d014814699111e80647a4dffafad2d4a1b0c69"

sys.path.insert(0, str(ROOT / "scripts"))

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
        self.assertEqual(len(list(INPUTS.rglob("*.yaml"))), 88)
        self.assertEqual(len(discover_model_paths(INPUTS)), 27)
        self.assertEqual(len(layers), 61)
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


if __name__ == "__main__":
    unittest.main()
