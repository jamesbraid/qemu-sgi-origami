# SPDX-License-Identifier: BSD-3-Clause

#!/usr/bin/env python3
"""Exercise independent machine exports and explicit adoption."""

import copy
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
INPUTS = ROOT / "hw/mips/sgi/models"
sys.path.insert(0, str(ROOT / "scripts"))
from sgi_model import model_qemu
from sgi_model.model_qemu import (
    generate_qemu_inputs, install_qemu_inputs,
    render_qemu_inputs,
)


class ExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.checked_models = model_qemu.load_checked_system_models(
            model_qemu.discover_model_paths(INPUTS), model_root=INPUTS)
        patcher = mock.patch.object(
            model_qemu, "load_checked_system_models",
            side_effect=lambda _paths, **_kwargs: copy.deepcopy(cls.checked_models))
        patcher.start()
        cls.addClassCleanup(patcher.stop)

    def test_checked_in_export_matches_model_inputs(self):
        for relative, expected in render_qemu_inputs(INPUTS).items():
            with self.subTest(path=relative):
                self.assertEqual((ROOT / relative).read_text(), expected)

    def test_selected_export_is_complete_and_repeatable(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = pathlib.Path(directory)
            paths = generate_qemu_inputs(INPUTS, tree, machines=("origin300",))
            self.assertEqual(
                {p.name for p in paths},
                {"sn-model.json", "sn-catalogue.json"})
            manifest = json.loads((tree / "hw/mips/sgi/sn-model.json").read_text())
            self.assertEqual(manifest["format"], 4)
            self.assertEqual(manifest["format_key"], "sgi-sn")
            for path, expected in manifest["outputs"].items():
                self.assertEqual(hashlib.sha256((tree / path).read_bytes()).hexdigest(),
                                 expected)
            before = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in paths}
            generate_qemu_inputs(INPUTS, tree, machines=("origin300",))
            self.assertEqual(before, {p: (p.read_bytes(), p.stat().st_mtime_ns)
                                      for p in paths})

    def test_default_topology_follows_the_products_base_composition(self):
        # Origin 2000's default is named for its module, not the product,
        # so a second compiled topology must not make its default ambiguous.
        product_models = {
            "origin2000": {"composition": {"product": "origin2000-rack"}}
        }
        model_qemu._check_default_topology(
            [{"name": "origin2000-rack"}, {"name": "origin2000-pci-shoehorn"}],
            {"origin2000-rack": "origin2000",
             "origin2000-pci-shoehorn": "origin2000"}, product_models)
        with self.assertRaisesRegex(ValueError, "default"):
            model_qemu._check_default_topology(
                [{"name": "origin2000-a"}, {"name": "origin2000-b"}],
                {"origin2000-a": "origin2000", "origin2000-b": "origin2000"},
                {"origin2000": {"composition": {"product": "origin2000"}}})

    def test_graphics_profiles_are_in_the_origin300_export(self):
        outputs = render_qemu_inputs(INPUTS, machines=("origin300",))
        catalogue = json.loads(outputs["hw/mips/sgi/sn-catalogue.json"])
        self.assertEqual(sorted(topology["name"] for topology in catalogue["topologies"]),
                         ["origin300", "origin300-pci-expansion-configuration-storage-experiment",
                          "origin300-pci-expansion-experiment",
                          "origin300-pci-expansion-pending-register-experiment",
                          "origin300-pci-expansion-unconnected-uart-experiment",
                          "origin300-two-chassis",
                          "origin300-v12-direct", "origin300-vbrick"])
        self.assertEqual({offering["topology"] for offering in catalogue["offerings"]},
                         {"origin300", "origin300-pci-expansion-configuration-storage-experiment",
                          "origin300-pci-expansion-experiment",
                          "origin300-pci-expansion-pending-register-experiment",
                          "origin300-pci-expansion-unconnected-uart-experiment",
                          "origin300-two-chassis",
                          "origin300-v12-direct", "origin300-vbrick"})

    def test_planned_target_stays_out_of_the_export(self):
        # A planned target compiles but the emulator cannot build it yet,
        # so the export leaves it out.
        selected = []
        for path, model in self.checked_models:
            if path.name == "origin300-two-chassis.yaml":
                model = copy.deepcopy(model)
                model["machine"]["emulation"]["lifecycle"] = "planned"
            selected.append((path, model))
        with mock.patch.object(model_qemu, "load_checked_system_models",
                               return_value=selected):
            outputs = render_qemu_inputs(INPUTS, machines=("origin300",))
        catalogue = json.loads(outputs["hw/mips/sgi/sn-catalogue.json"])
        self.assertEqual({topology["name"] for topology in catalogue["topologies"]},
                         {"origin300", "origin300-pci-expansion-configuration-storage-experiment",
                          "origin300-pci-expansion-experiment",
                          "origin300-pci-expansion-pending-register-experiment",
                          "origin300-pci-expansion-unconnected-uart-experiment",
                          "origin300-v12-direct", "origin300-vbrick"})
        self.assertEqual({offering["topology"] for offering in catalogue["offerings"]},
                         {"origin300", "origin300-pci-expansion-configuration-storage-experiment",
                          "origin300-pci-expansion-experiment",
                          "origin300-pci-expansion-pending-register-experiment",
                          "origin300-pci-expansion-unconnected-uart-experiment",
                          "origin300-v12-direct", "origin300-vbrick"})

    def test_bad_selection_does_not_create_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = pathlib.Path(directory) / "absent"
            with self.assertRaisesRegex(ValueError, "unsupported QEMU machine"):
                generate_qemu_inputs(ROOT, destination, machines=("unknown",))
            self.assertFalse(destination.exists())

    def test_adoption_requires_only_the_declared_format(self):
        with tempfile.TemporaryDirectory() as directory:
            tree = pathlib.Path(directory)
            with self.assertRaisesRegex(ValueError, "format declarations"):
                install_qemu_inputs(INPUTS, tree, machines=("origin300",))
            target = tree / "hw/mips"
            target.mkdir(parents=True)
            contract = target / "origami-model-formats.json"
            for declaration in ({}, {"sgi-sn": 1}, {"sgi-sn": 2}, {"sgi-sn": 3},
                                {"sgi-sn": True}):
                contract.write_text(json.dumps(declaration))
                with self.assertRaisesRegex(ValueError, "does not accept"):
                    install_qemu_inputs(INPUTS, tree, machines=("origin300",))
                self.assertFalse((target / "sgi/sn-catalogue.json").exists())
            contract.write_text('{"sgi-sn": 4}')
            install_qemu_inputs(INPUTS, tree, machines=("origin300",))
            self.assertTrue((target / "sgi/sn-catalogue.json").is_file())

    def test_firmware_path_metadata_is_preserved_in_export(self):
        original = next(model for path, model in self.checked_models
                        if path.name == "origin300.yaml")

        def export(model):
            selected = [
                (path, model if path.name == "origin300.yaml" else candidate)
                for path, candidate in self.checked_models
            ]
            with mock.patch.object(model_qemu, "load_checked_system_models",
                                   return_value=selected):
                outputs = render_qemu_inputs(INPUTS, machines=("origin300",))
            return (json.loads(outputs["hw/mips/sgi/sn-catalogue.json"]),
                    json.loads(outputs["hw/mips/sgi/sn-model.json"]))

        expected_catalogue, expected_manifest = export(original)
        changed = copy.deepcopy(original)
        changed["machine"]["emulation"]["firmware_payload"]["path"] = (
            "firmware/renamed.bin")
        actual_catalogue, actual_manifest = export(changed)
        self.assertNotEqual(actual_manifest["outputs"], expected_manifest["outputs"])
        self.assertNotEqual(actual_manifest["source_sha256"],
                            expected_manifest["source_sha256"])
        self.assertIn("firmware/renamed.bin", json.dumps(actual_catalogue))

    def test_origin2000_graphics_module_hubs_report_ni_revision_5(self):
        """`mgras_attach` (IRIX 6.5.30's SI Viz attach callback) reads
        NI_STATUS_REV_ID on the master node's Hub and refuses to attach
        below code 4; see docs/datasheets/hub-network-interface.md and
        docs/machines/layers/products/origin2000-rack.yaml's
        hub_ni_revision_overrides. Every Hub on Origin 2000's
        graphics-capable four-node module must compile to code 5, while
        Origin 200's node-board Hubs -- which reuse the same reusable
        ip27-node-board module but carry no override -- keep the general
        baseline, code 3, unaffected by the override existing at all.
        """
        outputs = render_qemu_inputs(INPUTS, machines=("origin2000", "origin200"))
        catalogue = json.loads(outputs["hw/mips/sgi/sn-catalogue.json"])
        topologies = {topology["name"]: topology for topology in catalogue["topologies"]}

        graphics_hubs = {
            node["id"]: node["identity"]["ni-revision"]
            for node in topologies["origin2000-rack"]["nodes"]
        }
        self.assertEqual(graphics_hubs, {
            "node-board-1-hub": 5, "node-board-2-hub": 5,
            "node-board-3-hub": 5, "node-board-4-hub": 5,
        })

        for name in ("origin200", "origin200-dual"):
            baseline_hubs = {
                node["id"]: node["identity"]["ni-revision"]
                for node in topologies[name]["nodes"]
            }
            self.assertTrue(baseline_hubs, f"{name} compiled no nodes")
            self.assertTrue(
                all(revision == 3 for revision in baseline_hubs.values()),
                f"{name} Hub(s) {baseline_hubs} should stay at the unoverridden "
                "general baseline")


if __name__ == "__main__":
    unittest.main()
