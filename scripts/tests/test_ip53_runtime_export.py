# SPDX-License-Identifier: BSD-3-Clause

#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["jsonschema>=4", "pyyaml>=6"]
# ///
"""Focused checks for the experimental IP53 catalogue export boundary."""

import pathlib
import sys
import unittest
import copy
import json

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from sgi_model.model_composition import validate_initialization_policy
from sgi_model.model_compile import CompileError, compile_model
from sgi_model.model_emit import (
    _is_ip53_runtime_profile,
    _validate_export_profiles,
    _validate_origin350_ip53_consoles,
    render_catalogue_json,
)
from sgi_model.model_validation import check_schema, load_checked_system_model, schema
from sgi_model.exceptions import ResolutionError
from sgi_model.registry import PRODUCT_MODELS


class IP53RuntimeExportTests(unittest.TestCase):
    def experiment_model(self):
        return {
            "composition": {"product": "origin350"},
            "machine": {
                "emulation": {"machine_name": "origin350",
                              "profile": "ip53-origin350"},
                "harness": {"launch_recipe": "ip53-discovery"},
            },
        }

    def test_experimental_pic_profile_is_scoped_to_named_product(self):
        model = self.experiment_model()
        model["components"] = [{"id": "pic", "kind": "pci-host-bridge",
                                "hardware_kind": "pic"}]
        model["machine"]["emulation"]["pic_profiles"] = {
            "pic": "pic-rev3-bedrock-experimental"}
        validate_initialization_policy(model)
        for field, value in (("product", "onyx350-base"),
                             ("machine_name", "origin300"),
                             ("launch_recipe", "other")):
            changed = self.experiment_model()
            changed["components"] = model["components"]
            changed["machine"]["emulation"].update(
                {"pic_profiles": model["machine"]["emulation"]["pic_profiles"]})
            if field == "product":
                changed["composition"][field] = value
            elif field == "machine_name":
                changed["machine"]["emulation"][field] = value
            else:
                changed["machine"]["harness"][field] = value
            with self.subTest(field=field), self.assertRaises(ResolutionError):
                validate_initialization_policy(changed)

    def test_fixture_profile_remains_valid_and_unknown_owner_is_refused(self):
        model = self.experiment_model()
        model["components"] = [{"id": "pic", "kind": "pci-host-bridge",
                                "hardware_kind": "pic"}]
        model["machine"]["emulation"]["pic_profiles"] = {
            "pic": "pic-rev3-bedrock"}
        validate_initialization_policy(model)
        model["machine"]["emulation"]["pic_profiles"] = {
            "missing": "pic-rev3-bedrock-experimental"}
        with self.assertRaisesRegex(ResolutionError, "unknown component"):
            validate_initialization_policy(model)

    def test_export_scope_requires_exact_product_machine_and_recipe(self):
        model = self.experiment_model()
        self.assertTrue(_is_ip53_runtime_profile(model))
        _validate_export_profiles(model, {"pci_host_bridges": [{
            "compatibility_profile": "pic-rev3-bedrock-experimental"}]})
        model["composition"]["product"] = "onyx350-base"
        self.assertFalse(_is_ip53_runtime_profile(model))
        with self.assertRaisesRegex(CompileError, "registered IP53 profile"):
            _validate_export_profiles(model, {"pci_host_bridges": [{
                "compatibility_profile": "pic-rev3-bedrock-experimental"}]})
        with self.assertRaisesRegex(CompileError, "qtest-only"):
            _validate_export_profiles(self.experiment_model(), {
                "pci_host_bridges": [{
                    "compatibility_profile": "pic-rev3-bedrock"}]})

    def test_five_ordered_serial_bindings_accept_both_primary_policies(self):
        for policy in ("l1-uart", "ip53-ioc4"):
            with self.subTest(policy=policy):
                model = self.experiment_model()
                primary_port = 3
                model["machine"]["harness"].update({
                    "console": policy,
                    "console_records": ([{
                        "kind": "console", "node_id": "node-0",
                        "argument": "-serial chardev:l1",
                        "launcher_name": "l1",
                    }] if policy == "l1-uart" else [{
                        "kind": "system-controller", "node_id": "node-0",
                        "argument": "-serial chardev:l1",
                        "launcher_name": "l1",
                    }]) + [{
                        "kind": ("console" if policy == "ip53-ioc4"
                                 and index == primary_port else "second-port"),
                        "node_id": "node-0",
                        "argument": f"-serial chardev:ioc4_serial_{index}",
                        "launcher_name": f"ioc4-serial-{index}",
                    } for index in range(4)],
                })
                _validate_origin350_ip53_consoles(
                    model, {"nodes": [{"id": "node-0"}]})

    def test_order_count_and_node_changes_are_rejected(self):
        model = self.experiment_model()
        harness = model["machine"]["harness"]
        harness.update({"console": "l1-uart", "console_records": [{
            "kind": "console", "node_id": "node-0",
            "argument": "-serial chardev:l1", "launcher_name": "l1",
        }] + [{
            "kind": "second-port", "node_id": "node-0",
            "argument": f"-serial chardev:ioc4_serial_{index}",
            "launcher_name": f"ioc4-serial-{index}",
        } for index in range(4)]})
        topology = {"nodes": [{"id": "node-0"}]}
        records = harness["console_records"]
        for altered in (records[:4], list(reversed(records)),
                        [dict(records[0], node_id="foreign")] + records[1:]):
            harness["console_records"] = altered
            with self.assertRaises(ValueError):
                _validate_origin350_ip53_consoles(model, topology)
        harness["console_records"] = records
        with self.assertRaises(ValueError):
            _validate_origin350_ip53_consoles(
                model, {"nodes": [{"id": "node-0"}, {"id": "node-1"}]})

    def test_r16000_identity_fields_are_limited_to_r16000_cpu(self):
        full_schema = schema(ROOT / "hw/mips/sgi/models")
        component_schema = {"$schema": full_schema["$schema"],
                            "$defs": full_schema["$defs"],
                            "$ref": "#/$defs/component"}
        import jsonschema
        validator = jsonschema.Draft202012Validator(component_schema)
        base = {"id": "cpu", "name": "CPU", "kind": "cpu",
                "provenance": "derived", "hardware_kind": "r16000",
                "identity": {"r16000_reset_mode": 0x009cad85,
                             "r16000_scache_bytes": 0x800000,
                             "required_properties": [
                                 "r16000-experimental", "r16000-reset-mode",
                                 "r16000-scache-bytes"]}}
        self.assertTrue(validator.is_valid(base))
        for changed in (
                dict(base, hardware_kind="r10000"),
                dict(base, kind="memory"),
                {**base, "identity": {
                    **base["identity"],
                    "required_properties": ["r16000-reset-mode"]}}):
            with self.subTest(changed=changed["kind"],
                              hw=changed["hardware_kind"]):
                self.assertFalse(validator.is_valid(changed))
        self.assertTrue(validator.is_valid({
            "id": "reference-cpu", "name": "CPU", "kind": "cpu",
            "provenance": "derived", "hardware_kind": "r16000",
        }))

    def test_origin350_and_tezro_reference_graphs_still_resolve(self):
        model_schema = schema(ROOT / "hw/mips/sgi/models")
        for filename, product in (("origin350.yaml", "origin350"),
                                  ("tezro-tower-one.yaml", "tezro-tower-one")):
            with self.subTest(filename=filename):
                model = load_checked_system_model(
                    ROOT / "hw/mips/sgi/models" / filename,
                    model_schema=model_schema)
                self.assertEqual(model["composition"]["product"], product)

    def test_tezro_tower_and_rack_base_compile_as_explicit_ip53_profiles(self):
        model_schema = schema(ROOT / "hw/mips/sgi/models")
        expected = {
            "tezro-tower-one": {"populations": [[1]], "pics": 1},
            "tezro-tower-multi": {"populations": [[2], [4]], "pics": 2},
            "tezro-rack-base": {"populations": [[1], [2], [4]], "pics": 1},
        }
        models = []
        for name, facts in expected.items():
            with self.subTest(name=name):
                model = load_checked_system_model(
                    ROOT / "hw/mips/sgi/models" / f"{name}.yaml",
                    model_schema=model_schema)
                self.assertEqual(model["machine"]["model_role"],
                                 "emulation-target")
                topology = compile_model(model)
                self.assertEqual(topology["name"], name)
                self.assertEqual(topology["generation"], "SN1")
                self.assertEqual(topology["supported_cpu_populations"],
                                 facts["populations"])
                self.assertEqual(len(topology["nodes"]), 1)
                self.assertTrue(all(
                    node["board_kind"] == "ip53" for node in topology["nodes"]
                ))
                self.assertEqual(len(topology["pci_host_bridges"]), facts["pics"])
                self.assertEqual(len(topology["io_boards"]), 1)
                if name in {"tezro-tower-one", "tezro-rack-base"}:
                    self.assertEqual(model["composition"]["population"]["cpus_per_node"], [1])
                    # These profiles author cpu0 as fitted. Rack-base also
                    # advertises two/four-CPU populations; its remaining
                    # compiled slots describe selectable capacity.
                    self.assertEqual(
                        [component["id"] for component in model["components"]
                         if component["kind"] == "cpu"], ["cpu0"])
                if name == "tezro-tower-one":
                    self.assertEqual(sum(
                        slot["assembly"] is not None
                        for slot in topology["nodes"][0]["cpu_slots"]), 1)
                models.append(model)

        product_models = {
            product: load_checked_system_model(
                ROOT / "hw/mips/sgi/models" / f"{filename}.yaml",
                model_schema=model_schema)
            for product, filename in PRODUCT_MODELS.items()
        }
        catalogue = json.loads(render_catalogue_json(
            models, product_models=product_models))
        offerings = [item for item in catalogue["offerings"]
                     if item["topology"] in expected]
        self.assertEqual(
            {(item["topology"], tuple(item["cpus-per-node"]))
             for item in offerings},
            {(name, tuple(population))
             for name, facts in expected.items()
             for population in facts["populations"]},
        )

    def test_ip53_board_identity_projects_without_changing_origin300(self):
        model_schema = schema(ROOT / "hw/mips/sgi/models")
        product_models = {
            product: load_checked_system_model(
                ROOT / "hw/mips/sgi/models" / f"{filename}.yaml",
                model_schema=model_schema)
            for product, filename in PRODUCT_MODELS.items()
        }
        for filename, expected, l1_type in (
                ("origin350.yaml", 0x2001, 0x37),
                ("origin300.yaml", 0x8001, 0x34)):
            with self.subTest(filename=filename):
                model = load_checked_system_model(
                    ROOT / "hw/mips/sgi/models" / filename,
                    model_schema=model_schema)
                export = json.loads(render_catalogue_json(
                    [model], product_models=product_models))
                topology = export["topologies"][0]
                word = topology["nodes"][0]["bedrock"]["board-id-word"]
                self.assertEqual(word, expected)
                controllers = topology["controllers"]
                self.assertEqual(len(controllers), 1)
                self.assertEqual(controllers[0]["kind"], "l1")
                self.assertEqual(controllers[0]["l1"]["reported-type"],
                                 l1_type)
                if filename == "origin350.yaml":
                    self.assertEqual((word >> 12) & 0xF, 2)
                    self.assertEqual(word & 0xFFF, 1)
                    self.assertEqual(topology["modules"][0]["nodes"], [0])
                    self.assertEqual(topology["modules"][0]["controller-index"],
                                     0)

    def test_integer_reset_mode_is_experiment_only(self):
        model_schema = schema(ROOT / "hw/mips/sgi/models")
        experiment = load_checked_system_model(
            ROOT / "hw/mips/sgi/models/origin350.yaml",
            model_schema=model_schema)
        self.assertEqual(experiment["machine"]["firmware_config"][
            "processor_reset_mode"], 0x009cad85)
        legacy = load_checked_system_model(
            ROOT / "hw/mips/sgi/models/origin200.yaml",
            model_schema=model_schema)
        invalid = copy.deepcopy(legacy)
        invalid["machine"]["firmware_config"]["processor_reset_mode"] = 0x009cad85
        self.assertTrue(check_schema(invalid, model_schema))
        for selector, mutate in (
                ("missing product", lambda model: model["composition"].pop("product")),
                ("wrong product", lambda model: model["composition"].update(
                    product="onyx350-base")),
                ("missing generation", lambda model: model["machine"].pop("generation")),
                ("wrong generation", lambda model: model["machine"].update(
                    generation="SN0")),
                ("missing class", lambda model: model["machine"]["emulation"].pop(
                    "machine_name")),
                ("wrong class", lambda model: model["machine"]["emulation"].update(
                    machine_name="origin300")),
                ("missing recipe", lambda model: model["machine"]["harness"].pop(
                    "launch_recipe")),
                ("wrong recipe", lambda model: model["machine"]["harness"].update(
                    launch_recipe="other")),
                ("missing profile", lambda model: model["machine"]["emulation"].pop(
                    "pic_profiles")),
                ("wrong profile", lambda model: model["machine"]["emulation"].update(
                    pic_profiles={"pic": "pic-rev3-bedrock"})),):
            changed = copy.deepcopy(experiment)
            mutate(changed)
            with self.subTest(selector=selector):
                self.assertTrue(check_schema(changed, model_schema))


if __name__ == "__main__":
    unittest.main()
