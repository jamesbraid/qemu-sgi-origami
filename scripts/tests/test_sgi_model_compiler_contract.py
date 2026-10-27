# SPDX-License-Identifier: BSD-3-Clause

#!/usr/bin/env python3
"""Tests for the format-2 compiler and compiled-catalogue emitter.

Record-shape tests inspect the typed graph index of resolved model fixtures
and compare compiled fields with expected values. Golden tests capture each
catalogued configuration's compiled JSON and manifest, or its refusal
message, under system-model-fixtures/format2/<configuration>/.
ORIGAMI_UPDATE_GOLDENS=1 recaptures those files.

The Origin 200 tests also check its compiled node, processor, flash and I/O
board fields directly. These checks read compile_model's untrimmed result;
CatalogueJsonTests checks the separately rendered catalogue output.
"""

from __future__ import annotations

import copy
import json
import os
import pathlib
import sys
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURES = pathlib.Path(__file__).resolve().parent / "system-model-fixtures"
GOLDEN_ROOT = FIXTURES / "format2"
MACHINES = ROOT / "hw/mips/sgi/models"

sys.path.insert(0, str(ROOT / "scripts"))

from sgi_model.model_compile import (
    CompileError, _mode_bits, _processor, compile_model, index_graph,
)
from sgi_model.model_emit import _io_board as _emit_io_board
from sgi_model.model_emit import _ip53_console_records
from sgi_model.model_emit import _offerings_for
from sgi_model.model_emit import _resources_for
from sgi_model.model_emit import render_catalogue_json as _render_catalogue_json, render_manifest
from sgi_model.model_validation import load_checked_system_model, schema
from sgi_model.registry import PRODUCT_MODELS


# These tests repeatedly resolve the same model fixture files. Most read
# fresh, unmutated copies; one test point
# (test_unsupported_generation_is_refused_by_name) mutates the model it
# gets back. Resolving each configuration once here and handing every
# caller its own deep copy keeps that mutation local while dropping the
# repeated schema-checked resolution, which is the real cost.
_RESOLVED_CACHE: dict[str, dict] = {}


def _resolved(name: str) -> dict:
    if name not in _RESOLVED_CACHE:
        _RESOLVED_CACHE[name] = load_checked_system_model(MACHINES / f"{name}.yaml", model_schema=schema(MACHINES))
    return copy.deepcopy(_RESOLVED_CACHE[name])

def render_catalogue_json(models: list[dict]) -> str:
    product_models = {
        product: _resolved(model_name)
        for product, model_name in PRODUCT_MODELS.items()
    }
    return _render_catalogue_json(models, product_models=product_models)


class IP53ConsoleOfferingTests(unittest.TestCase):
    def harness(self, records):
        return {
            "console": "ip53-ioc4",
            "launch_recipe": "ip53-discovery",
            "console_records": records,
        }

    def test_console_records_follow_stable_owner_through_filter_and_reorder(self):
        records = [
            {"kind": "second-port", "node_id": "early-node",
             "argument": "-serial chardev:ioc4_serial_1",
             "launcher_name": "ioc4-serial-1"},
            {"kind": "console", "node_id": "late-node",
             "argument": "-serial chardev:ioc4_serial_0",
             "launcher_name": "ioc4-serial-0"},
        ]
        topology = {"nodes": [{"id": "early-node"}, {"id": "late-node"}]}
        selected = _ip53_console_records(topology, self.harness(records), (1,))
        self.assertEqual(selected, [{
            "kind": "console", "node": 1,
            "argument": "-serial chardev:ioc4_serial_0",
            "launcher-name": "ioc4-serial-0",
        }])

        reordered = {"nodes": [{"id": "late-node"}, {"id": "early-node"}]}
        moved = _ip53_console_records(reordered, self.harness(records), (0,))
        self.assertEqual(moved, [{
            "kind": "console", "node": 0,
            "argument": "-serial chardev:ioc4_serial_0",
            "launcher-name": "ioc4-serial-0",
        }])

    def test_population_without_named_primary_is_rejected(self):
        records = [
            {"kind": "second-port", "node_id": "early-node",
             "argument": "-serial chardev:ioc4_serial_1",
             "launcher_name": "ioc4-serial-1"},
            {"kind": "console", "node_id": "late-node",
             "argument": "-serial chardev:ioc4_serial_0",
             "launcher_name": "ioc4-serial-0"},
        ]
        topology = {"nodes": [{"id": "early-node"}, {"id": "late-node"}]}
        with self.assertRaisesRegex(ValueError, "one named primary"):
            _ip53_console_records(topology, self.harness(records), (0,))

    def test_duplicate_console_names_and_bindings_are_rejected(self):
        base = {"kind": "console", "node_id": "node-a",
                "argument": "-serial chardev:ioc4_serial_0",
                "launcher_name": "ioc4-serial-0"}
        topology = {"nodes": [{"id": "node-a"}]}
        duplicate_name = [base, {**base, "argument":
                                 "-serial chardev:ioc4_serial_1"}]
        duplicate_binding = [base, {**base, "kind": "second-port",
                                    "launcher_name": "ioc4-serial-1"}]
        for records in (duplicate_name, duplicate_binding):
            with self.subTest(records=records):
                with self.assertRaisesRegex(ValueError, "duplicate"):
                    _ip53_console_records(
                        topology, self.harness(records), (0,))

    def test_duplicate_names_are_checked_for_each_selected_population(self):
        records = [
            {"kind": "console", "node_id": "node-a",
             "argument": "-serial chardev:ioc4_serial_0",
             "launcher_name": "ioc4-serial-0"},
            {"kind": "second-port", "node_id": "node-a",
             "argument": "-serial chardev:ioc4_serial_1",
             "launcher_name": "shared-name"},
            {"kind": "second-port", "node_id": "node-b",
             "argument": "-serial chardev:ioc4_serial_2",
             "launcher_name": "shared-name"},
        ]
        topology = {"nodes": [{"id": "node-a"}, {"id": "node-b"}]}
        selected = _ip53_console_records(
            topology, self.harness(records), (0,))
        self.assertEqual([record["launcher-name"] for record in selected],
                         ["ioc4-serial-0", "shared-name"])
        with self.assertRaisesRegex(ValueError, "duplicate name in this offering"):
            _ip53_console_records(topology, self.harness(records), (0, 1))

    def test_console_record_rejects_unknown_owner_and_arbitrary_argument(self):
        records = [{"kind": "console", "node_id": "missing-node",
                    "argument": "-serial chardev:ioc4_serial_0",
                    "launcher_name": "ioc4-serial-0"}]
        topology = {"nodes": [{"id": "node-a"}]}
        with self.assertRaisesRegex(ValueError, "unknown node id"):
            _ip53_console_records(topology, self.harness(records), (0,))
        records[0]["node_id"] = "node-a"
        records[0]["argument"] = "-device ioc4"
        with self.assertRaisesRegex(ValueError, "serial chardev binding"):
            _ip53_console_records(topology, self.harness(records), (0,))

    def test_explicit_l1_primary_matches_policy_and_binding(self):
        records = [{"kind": "console", "node_id": "node-a",
                    "argument": "-serial chardev:l1", "launcher_name": "l1"}]
        topology = {"nodes": [{"id": "node-a"}]}
        self.assertEqual(_ip53_console_records(
            topology, {"launch_recipe": "ip53-discovery", "console": "l1-uart",
                     "console_records": records}, (0,)), [{
                         "kind": "console", "node": 0,
                         "argument": "-serial chardev:l1", "launcher-name": "l1",
                     }])

    def test_l1_policy_rejects_mismatched_primary_name_or_binding(self):
        topology = {"nodes": [{"id": "node-a"}]}
        for name, argument in (("l1", "-serial chardev:ioc4_serial_0"),
                               ("ioc4-serial-0", "-serial chardev:l1")):
            with self.subTest(name=name, argument=argument):
                record = {"kind": "console", "node_id": "node-a",
                          "argument": argument, "launcher_name": name}
                with self.assertRaisesRegex(ValueError, "L1 record"):
                    _ip53_console_records(
                        topology, {"launch_recipe": "ip53-discovery",
                                   "console": "l1-uart",
                                   "console_records": [record]}, (0,))

    def test_console_policy_must_match_primary_ioc4_binding(self):
        records = [{"kind": "console", "node_id": "node-a",
                    "argument": "-serial chardev:l1",
                    "launcher_name": "l1"}]
        topology = {"nodes": [{"id": "node-a"}]}
        with self.assertRaisesRegex(ValueError, "policy must match"):
            _ip53_console_records(topology, self.harness(records), (0,))

    def test_primary_launcher_name_must_match_chardev_binding(self):
        records = [{"kind": "console", "node_id": "node-a",
                    "argument": "-serial chardev:ioc4_serial_1",
                    "launcher_name": "ioc4-serial-0"}]
        topology = {"nodes": [{"id": "node-a"}]}
        with self.assertRaisesRegex(ValueError, "policy must match"):
            _ip53_console_records(topology, self.harness(records), (0,))

    def test_legacy_offerings_omit_new_optional_fields(self):
        catalogue = json.loads(render_catalogue_json([_resolved("origin300")]))
        offerings = [item for item in catalogue["offerings"]
                     if item["product"] == "origin300"]
        self.assertTrue(offerings)
        model = _resolved("origin300")
        topology = compile_model(model)
        model["machine"]["harness"]["console"] = "l1-uart"
        product_models = {product: _resolved(model_name)
                          for product, model_name in PRODUCT_MODELS.items()}
        projected = _offerings_for(model, topology, product_models)
        self.assertEqual(projected[0]["console"],
                         product_models["origin300"]["machine"]["harness"]["console"])
        for offering in offerings:
            self.assertNotIn("launch-recipe", offering)
            self.assertTrue(all("launcher-name" not in console
                                for console in offering["consoles"]))

    def test_console_records_reject_invalid_roles_and_policy_scope(self):
        topology = {"nodes": [{"id": "node-a"}]}
        record = {"kind": "console", "node_id": "node-a",
                  "argument": "-serial chardev:ioc4_serial_0",
                  "launcher_name": "ioc4-serial-0"}
        with self.assertRaisesRegex(ValueError, "unsupported role"):
            _ip53_console_records(
                topology, self.harness([{**record, "kind": "unlisted"}]),
                (0,))
        with self.assertRaisesRegex(ValueError, "L1 record"):
            _ip53_console_records(
                topology, {**self.harness([record]), "console": "l1-uart"},
                (0,))

    def test_origin350_experiment_r16000_projects_explicit_reset_and_cache(self):
        model = {
            "composition": {"product": "origin350"},
            "machine": {"emulation": {"machine_name": "origin350"}},
        }
        cpu = {
            "id": "cpu0", "hardware_kind": "r16000",
            "identity": {
                "clock_mhz": 700, "cache_mib": 8,
                "r16000_reset_mode": 0x009cad85,
                "r16000_scache_bytes": 0x800000,
                "required_properties": [
                    "r16000-experimental", "r16000-reset-mode",
                    "r16000-scache-bytes",
                ],
            },
        }

        processor = _processor(model, cpu)
        processor["mode_bits"] = _mode_bits(model, {}, processor)

        self.assertEqual(processor, {
            "id": "r16000-700mhz", "kind": "R16000",
            "clock_hz": 700_000_000, "cache_mib": 8,
            "mode_bits": 0x009cad85,
            "required_properties": [
                "r16000-experimental", "r16000-reset-mode",
                "r16000-scache-bytes",
            ],
        })

    def test_r16000_projection_is_limited_to_origin350_experiment(self):
        model = {
            "composition": {"product": "origin300"},
            "machine": {"emulation": {"machine_name": "origin300"}},
        }
        cpu = {
            "id": "cpu0", "hardware_kind": "r16000",
            "identity": {
                "clock_mhz": 700, "cache_mib": 8,
                "r16000_reset_mode": 0x009cad85,
                "r16000_scache_bytes": 0x800000,
                "required_properties": [
                    "r16000-experimental", "r16000-reset-mode",
                    "r16000-scache-bytes",
                ],
            },
        }

        with self.assertRaisesRegex(CompileError, "only the Origin 350 IP53 experiment"):
            _processor(model, cpu)

    def test_r16000_projection_rejects_mismatched_and_missing_inputs(self):
        model = {
            "composition": {"product": "origin350"},
            "machine": {"emulation": {"machine_name": "origin350"}},
        }
        cpu = {
            "id": "cpu0", "hardware_kind": "r16000",
            "identity": {
                "clock_mhz": 700, "cache_mib": 8,
                "r16000_reset_mode": 0x009cad85,
                "r16000_scache_bytes": 0x800000,
                "required_properties": [
                    "r16000-experimental", "r16000-reset-mode",
                    "r16000-scache-bytes",
                ],
            },
        }
        missing = object()
        cases = (
            ("machine class", ("machine", "emulation", "machine_name"), "origin300"),
            ("product class", ("composition", "product"), "onyx350-base"),
            ("clock", ("identity", "clock_mhz"), 701),
            ("cache", ("identity", "cache_mib"), 4),
            ("reset mode", ("identity", "r16000_reset_mode"), 0x009aa985),
            ("secondary cache bytes", ("identity", "r16000_scache_bytes"), 0x400000),
            ("boolean clock", ("identity", "clock_mhz"), True),
            ("string clock", ("identity", "clock_mhz"), "700"),
            ("boolean cache", ("identity", "cache_mib"), True),
            ("string cache", ("identity", "cache_mib"), "8"),
            ("boolean reset mode", ("identity", "r16000_reset_mode"), True),
            ("string reset mode", ("identity", "r16000_reset_mode"), "0x009cad85"),
            ("boolean cache bytes", ("identity", "r16000_scache_bytes"), True),
            ("string cache bytes", ("identity", "r16000_scache_bytes"), "0x800000"),
            ("missing clock", ("identity", "clock_mhz"), missing),
            ("missing cache", ("identity", "cache_mib"), missing),
            ("missing reset mode", ("identity", "r16000_reset_mode"), missing),
            ("missing secondary cache bytes", ("identity", "r16000_scache_bytes"), missing),
            ("missing QOM requirements", ("identity", "required_properties"), missing),
            ("reordered QOM requirements", ("identity", "required_properties"), [
                "r16000-reset-mode", "r16000-experimental",
                "r16000-scache-bytes",
            ]),
            ("missing experimental QOM requirement", ("identity", "required_properties"), [
                "r16000-reset-mode", "r16000-scache-bytes",
            ]),
        )
        for field, path, value in cases:
            with self.subTest(field=field):
                candidate_model, candidate_cpu = copy.deepcopy(model), copy.deepcopy(cpu)
                target = (candidate_model if path[0] != "identity"
                          else candidate_cpu["identity"])
                keys = path if path[0] != "identity" else path[1:]
                for key in keys[:-1]:
                    target = target[key]
                if value is missing:
                    target.pop(keys[-1])
                else:
                    target[keys[-1]] = value
                with self.assertRaises(ValueError):
                    _processor(candidate_model, candidate_cpu)

    def test_ip53_recipe_and_ioc4_or_l1_policy_are_emitted_for_each_population(self):
        model = _resolved("origin300-two-chassis")
        topology = compile_model(model)
        topology["node_populations"] = [
            {"grade": "SUPPORTED", "nodes": [0],
             "supported_cpu_populations": [[1]]},
            {"grade": "SUPPORTED", "nodes": [0, 1],
             "supported_cpu_populations": [[1, 1]]},
        ]
        product_models = {
            product: _resolved(model_name)
            for product, model_name in PRODUCT_MODELS.items()
        }
        selected_harness = model["machine"].setdefault("harness", {})
        selected_harness.update(self.harness([
            {"kind": "console", "node_id": topology["nodes"][0]["id"],
             "argument": "-serial chardev:ioc4_serial_0",
             "launcher_name": "ioc4-serial-0"},
            {"kind": "second-port", "node_id": topology["nodes"][1]["id"],
             "argument": "-serial chardev:ioc4_serial_1",
             "launcher_name": "ioc4-serial-1"},
        ]))

        offerings = _offerings_for(model, topology, product_models)
        by_nodes = {offering["nodes"]: offering for offering in offerings}
        self.assertEqual(by_nodes[1]["launch-recipe"], "ip53-discovery")
        self.assertEqual(by_nodes[1]["console"], "ip53-ioc4")
        self.assertEqual([item["launcher-name"]
                          for item in by_nodes[1]["consoles"]],
                         ["ioc4-serial-0"])
        self.assertEqual([item["launcher-name"]
                          for item in by_nodes[2]["consoles"]],
                         ["ioc4-serial-0", "ioc4-serial-1"])

        selected_harness.update(self.harness([{
            "kind": "console", "node_id": topology["nodes"][0]["id"],
            "argument": "-serial chardev:l1", "launcher_name": "l1",
        }]))
        selected_harness["console"] = "l1-uart"
        l1_offerings = _offerings_for(model, topology, product_models)
        self.assertTrue(all(offering["console"] == "l1-uart" and
                            offering["consoles"] == [{
                                "kind": "console", "node": 0,
                                "argument": "-serial chardev:l1",
                                "launcher-name": "l1",
                            }] for offering in l1_offerings))



class ComputeOptionIntxTests(unittest.TestCase):
    def test_catalogue_preserves_compute_option_maps(self):
        model = _resolved("origin300-numalink-pci-expansion")
        topology = json.loads(render_catalogue_json([model]))["topologies"][0]
        nodes = topology["nodes"]
        for node in nodes:
            self.assertEqual(node.get("option-intx-map"),
                             [1, 5, 1, 5, 2, 6, 2, 6])

    def test_unknown_compute_option_owner_is_refused(self):
        model = _resolved("origin300-numalink-pci-expansion")
        model["machine"]["emulation"]["option_intx_maps"]["missing"] = [1] * 8
        with self.assertRaises(CompileError):
            compile_model(model)

    def test_selected_experiment_has_independent_option_maps(self):
        model = _resolved("origin300-numalink-pci-expansion")
        model["machine"]["emulation"]["option_intx_maps"] = {
            "master-bedrock": [1, 5, 1, 5, 2, 6, 2, 6],
            "slave-bedrock": [2, 6, 2, 6, 1, 5, 1, 5],
        }
        nodes = compile_model(model)["nodes"]
        self.assertEqual(nodes[0].get("option_intx_map"),
                         [1, 5, 1, 5, 2, 6, 2, 6])
        self.assertEqual(nodes[1].get("option_intx_map"),
                         [2, 6, 2, 6, 1, 5, 1, 5])

    def test_ordinary_origin300_has_no_option_map(self):
        for node in compile_model(_resolved("origin300"))["nodes"]:
            self.assertNotIn("option_intx_map", node)

    def test_unselected_compute_option_map_is_refused(self):
        model = _resolved("origin300")
        model["machine"]["emulation"]["option_intx_maps"] = {
            "bedrock": [1, 5, 1, 5, 2, 6, 2, 6],
        }
        with self.assertRaises(CompileError):
            compile_model(model)

    def test_invalid_compute_option_map_is_refused(self):
        for value in ([1] * 7, [8] * 8, [True] * 8):
            model = _resolved("origin300-numalink-pci-expansion")
            model["machine"]["emulation"]["option_intx_maps"] = {
                "master-bedrock": value,
            }
            with self.assertRaises(CompileError):
                compile_model(model)


class LaunchPolicyFieldTests(unittest.TestCase):
    """The catalogue fields the emulator and launchers read in place of
    comparing topology names."""

    def test_router_diagnostics_reach_every_router_of_their_topology(self):
        model = _resolved("origin300-numalink-pci-expansion")
        topology = json.loads(render_catalogue_json([model]))["topologies"][0]
        self.assertTrue(topology["routers"])
        for router in topology["routers"]:
            self.assertIs(router.get("synthetic-diagnostics"), True)

    def test_router_diagnostics_need_sn1_routers(self):
        model = _resolved("origin300")
        model["machine"]["emulation"]["synthetic_router_diagnostics"] = True
        with self.assertRaisesRegex(CompileError, "SN1 Routers"):
            compile_model(model)

    def test_ip53_runtime_is_emitted_as_authored(self):
        model = _resolved("tezro-tower-one")
        topology = json.loads(render_catalogue_json([model]))["topologies"][0]
        self.assertEqual(topology["ip53-runtime"], {
            "pic-profile": "pic-rev3-bedrock-tezro-experimental",
            "normal-selection": {
                "cpus": 1, "fitted-cpus": 1, "memory-mib": 4096,
                "clock-hz": 700_000_000, "cache-mib": 4,
                "mode-word": 0x009bad85,
            },
            "l1-irouter-compat": True,
        })

    def test_ip53_runtime_pic_profile_must_match_its_hosts(self):
        model = _resolved("origin350")
        model["machine"]["emulation"]["ip53_runtime"]["pic_profile"] = (
            "pic-rev3-bedrock-tezro-experimental")
        with self.assertRaisesRegex(CompileError, "does not select"):
            compile_model(model)

    def test_ip53_runtime_needs_ip53_nodes(self):
        model = _resolved("origin300")
        model["machine"]["emulation"]["ip53_runtime"] = copy.deepcopy(
            _resolved("origin350")["machine"]["emulation"]["ip53_runtime"])
        with self.assertRaisesRegex(CompileError, "IP53 node boards"):
            compile_model(model)

    def test_normal_offering_follows_the_normal_selection(self):
        def offerings(model):
            catalogue = json.loads(render_catalogue_json([model]))
            return catalogue["offerings"]

        model = _resolved("origin350")
        self.assertTrue(all("required-properties" not in item["processor"]
                            for item in offerings(model)))
        del model["machine"]["emulation"]["ip53_runtime"]["normal_selection"]
        self.assertTrue(all("required-properties" in item["processor"]
                            for item in offerings(model)))

    def test_each_product_has_one_machine_default(self):
        models = [_resolved(name) for name in sorted({
            *PRODUCT_MODELS.values(), "origin200-dual",
            "tezro-tower-one", "onyx2-deskside"})]
        topologies = json.loads(render_catalogue_json(models))["topologies"]
        defaults = {}
        for topology in topologies:
            if topology.get("machine-default"):
                self.assertNotIn(topology["product"], defaults)
                defaults[topology["product"]] = topology["name"]
        self.assertEqual(set(defaults), set(PRODUCT_MODELS))
        self.assertEqual(defaults["origin2000"], "origin2000-rack")


class BedrockConfigurationTests(unittest.TestCase):
    def catalogue(self, model):
        return json.loads(render_catalogue_json([model]))["topologies"][0]

    def test_origin300_initialization_comes_from_configuration(self):
        model = _resolved("origin300")
        policy = model["machine"]["emulation"]["bedrock_nodes"]["bedrock"]
        policy["board_id_word"] = 0x9001
        policy["local_region"] = 3
        model["machine"]["emulation"]["l1_controllers"]["l1"].update({
            "reported_type": 0x35,
            "revision": {"major": 2, "minor": 3, "patch": 4},
        })
        bedrock = next(c for c in model["components"] if c["id"] == "bedrock")
        bedrock["identity"]["hardware_revision"] = 3
        topology = self.catalogue(model)
        node = topology["nodes"][0]
        self.assertEqual(node["bedrock"]["board-id-word"], 0x9001)
        self.assertEqual(node["bedrock"]["local-region"], 3)
        self.assertEqual(node["bedrock"]["installed-bank-mask"], 0x0c)
        self.assertEqual(node["identity"]["widget-revision"], 3)
        self.assertEqual(topology["controllers"][0]["l1"], {
            "reported-type": 0x35,
            "revision": {"major": 2, "minor": 3, "patch": 4},
        })

    def test_fuel_board_values_come_from_its_catalogue(self):
        topology = self.catalogue(_resolved("fuel"))
        node = topology["nodes"][0]
        self.assertEqual(node["identity"]["widget-revision"], 2)
        self.assertEqual(node["bedrock"], {
            "board-id-word": 0x4004, "board-id-mask": 0xf000,
            "board-id-required-bits": 0x4000, "initiator-region": 0,
            "installed-bank-mask": 3, "local-region": 0})
        # The L1 revision stays unknown: no capture fits the selected board.
        l1 = topology["controllers"][0]["l1"]
        self.assertEqual(l1, {
            "reported-type": 0x41,
            "mac-eeprom-input": {
                "option": "fuel-mac-eeprom", "required": False}})
        board = topology["io-boards"][0]
        self.assertTrue(board["populated"])
        self.assertEqual(board["mac-source"], "module-l1")
        self.assertEqual(board["ioc3-subsystem-id"], 0xc30a)
        self.assertEqual(board["pci-functions"], [
            {"kind": "isp12160", "revision": 6},
            {"kind": "ioc3", "revision": 1},
            {"kind": "lucent-ohci", "revision": 16},
        ])

    def test_fuel_fits_its_vpro_at_widget_13(self):
        fuel = self.catalogue(_resolved("fuel"))
        boards = {board["preset"]: board for board in fuel["io-boards"]}
        attachment = fuel["xio-attachments"][boards["odyssey"]["attachment"]]
        self.assertEqual(attachment["widget"], 13)
        # A launcher does not offer a Fuel without it.
        self.assertFalse(fuel["graphics"]["headless"])
        self.assertEqual(fuel["graphics"]["headless-refused"],
                         "every Fuel has its VPro graphics slot at widget 13")

    def test_fitted_assembly_presence_does_not_depend_on_serial(self):
        model = _resolved("origin300")
        from sgi_model.model_compile import _synthetic_serial
        with mock.patch("sgi_model.model_compile._synthetic_serial",
                        side_effect=lambda model, cid, *args: (
                            None if cid == "ioc3"
                            else _synthetic_serial(model, cid, *args))):
            entry = compile_model(model)
        self.assertFalse(entry["io_boards"][0]["populated"])
        self.assertTrue(entry["io_boards"][1]["populated"])

    def test_board_values_take_no_input_binding(self):
        # Overrides are family machine properties, not per-field bindings.
        for section, owner, field in (
                ("bedrock_nodes", "bedrock", "board_id_input"),
                ("bedrock_nodes", "bedrock", "revision_input"),
                ("l1_controllers", "l1", "reported_type_input"),
                ("l1_controllers", "l1", "revision_input"),
                ("io_boards", "xbridge-bridge-f", "ioc3_subsystem_input")):
            with self.subTest(field=field):
                model = _resolved("origin300")
                model["machine"]["emulation"][section][owner][field] = {
                    "option": "board-id-word", "required": False}
                with self.assertRaisesRegex(ValueError, "invalid"):
                    compile_model(model)

    def test_malformed_configuration_is_rejected(self):
        mutations = [
            ("bedrock_nodes", "bedrock", "board_id_input",
             {"option": "fuel-l1-type-code", "required": True}),
            ("bedrock_nodes", "bedrock", "local_region", -1),
            ("bedrock_nodes", "bedrock", "board_id_mask", 0xf000),
            ("l1_controllers", "l1", "reported_type", 0),
            ("l1_controllers", "l1", "revision", {"major": 1}),
            ("l1_controllers", "l1", "revision",
             {"major": 2147483648, "minor": 0, "patch": 0}),
            ("bedrock_nodes", "bedrock", "initiator_region", 64),
            ("l1_controllers", "l1", "revision",
             {"major": 1, "minor": -1, "patch": 1}),
        ]
        for section, owner, field, value in mutations:
            with self.subTest(field=field):
                model = _resolved("origin300")
                model["machine"]["emulation"][section][owner][field] = value
                with self.assertRaises(ValueError):
                    compile_model(model)

    def test_unsupported_pci_revision_is_rejected(self):
        model = _resolved("origin300")
        ioc3 = next(c for c in model["components"] if c["id"] == "ioc3")
        ioc3["identity"]["pci_revision"] = 2
        with self.assertRaisesRegex(ValueError, "revision"):
            compile_model(model)


    def test_populated_bedrock_board_requires_declared_clock_policy(self):
        for name in ("fuel", "origin300"):
            with self.subTest(machine=name):
                model = _resolved(name)
                model["machine"]["emulation"].pop("system_timekeepers")
                with self.assertRaisesRegex(ValueError, "Timekeeper"):
                    compile_model(model)

    def test_local_region_limit_matches_device(self):
        model = _resolved("origin300")
        model["machine"]["emulation"]["bedrock_nodes"]["bedrock"][
            "local_region"] = 64
        with self.assertRaisesRegex(ValueError, "local_region"):
            compile_model(model)

    def test_malformed_smdk_recipe_is_rejected(self):
        for field, value in (("tftp_mode", "unknown"), ("package_path", ""),
                             ("diagnostic_mac_bootarg", 1),
                             ("discovery_prerequisites", ["io8bt", "io8bt"])):
            with self.subTest(field=field):
                model = _resolved("fuel")
                model["machine"]["harness"]["smdk"][field] = value
                with self.assertRaisesRegex(ValueError, "sMDK"):
                    compile_model(model)

    def test_smdk_recipe_follows_selected_configuration(self):
        model = _resolved("origin300")
        model["composition"]["product"] = "origin300-requested"
        model["machine"]["harness"]["smdk"] = {
            "package_path": "assets/boot/smdk-variant",
            "tftp_mode": "requested",
            "diagnostic_mac_bootarg": True, "requires_boot_flash": True,
            "requires_network_identity": True,
            "discovery_prerequisites": ["launcher"],
        }
        catalogue = json.loads(
            render_catalogue_json([model, _resolved("fuel")]))
        changed = next(item for item in catalogue["offerings"]
                       if item["topology"] == "origin300-requested")
        self.assertEqual(changed["smdk"], {
            "package-path": "assets/boot/smdk-variant",
            "tftp-mode": "requested",
            "diagnostic-mac-bootarg": True, "requires-boot-flash": True,
            "requires-network-identity": True,
            "discovery-prerequisites": ["launcher"],
        })
        fuel = next(item for item in catalogue["offerings"]
                    if item["topology"] == "fuel")
        self.assertEqual(
            fuel["smdk"]["package-path"], "assets/boot/smdk-fuel-6526")

    def test_signed_l1_revision_boundary_is_preserved(self):
        model = _resolved("origin300")
        model["machine"]["emulation"]["l1_controllers"]["l1"]["revision"] = {
            "major": 2147483647, "minor": 0, "patch": 0,
        }
        self.assertEqual(self.catalogue(model)["controllers"][0]["l1"][
            "revision"]["major"], 2147483647)

    def test_disconnected_product_board_does_not_select_node_assembly(self):
        model = _resolved("origin300")
        model["components"].append({
            "id": "unconnected-board", "name": "Unconnected board",
            "kind": "board",
            "hardware_kind": "ip34", "provenance": "derived",
        })
        boards = compile_model(model)["io_boards"]
        self.assertEqual(
            [board["board_kind"] for board in boards], ["IO8", "IO8"])
        self.assertEqual(
            [board["populated"] for board in boards], [False, True])

    def test_missing_board_values_are_rejected(self):
        # No product may leave a board value to a launch option.
        for name in ("origin300", "fuel"):
            for section, owner, field in (
                    ("bedrock_nodes", "bedrock", "board_id_word"),
                    ("l1_controllers", "l1", "reported_type"),
                    ("io_boards", None, "ioc3_subsystem_id")):
                with self.subTest(machine=name, field=field):
                    model = _resolved(name)
                    policies = model["machine"]["emulation"][section]
                    owner = owner or next(iter(policies))
                    del policies[owner][field]
                    with self.assertRaisesRegex(ValueError,
                                                f"catalogue {field}"):
                        compile_model(model)
            with self.subTest(machine=name, field="hardware_revision"):
                model = _resolved(name)
                bedrock = next(c for c in model["components"]
                               if c["id"] == "bedrock")
                del bedrock["identity"]["hardware_revision"]
                with self.assertRaisesRegex(ValueError,
                                            "catalogue hardware_revision"):
                    compile_model(model)
        model = _resolved("origin300")
        model["machine"]["emulation"].pop("l1_controllers")
        with self.assertRaisesRegex(ValueError, "configured L1"):
            compile_model(model)

    def test_binding_aliases_have_one_owner(self):
        model = _resolved("origin300-two-chassis")
        for entry in model["machine"]["emulation"]["l1_controllers"].values():
            entry["mac_eeprom_input"] = {
                "option": "fuel-mac-eeprom", "required": False}
        with self.assertRaisesRegex(ValueError, "multiple owners"):
            compile_model(model)

    def test_offering_inputs_follow_selected_configuration(self):
        catalogue = json.loads(render_catalogue_json(
            [_resolved("fuel"), _resolved("origin300")]))
        offerings = catalogue["offerings"]
        fuel = next(item for item in offerings if item["topology"] == "fuel")
        self.assertEqual(fuel["hardware-inputs"], [
            {"option": "fuel-mac-eeprom", "required": False}])
        for offering in offerings:
            if offering["topology"] == "origin300":
                self.assertEqual(offering["hardware-inputs"], [])


class Ip53CpuContextTests(unittest.TestCase):
    def test_full_firmware_word_and_node_clocks_reach_the_l1(self):
        for name, word, cpu_hz, reset in (
                ("origin350", 0x569cad85, 700_000_000, 0x009cad85),
                ("onyx350-compat", 0x569caf85, 800_000_000, 0x009caf85),
                ("tezro-tower-one", 0x549bad85, 700_000_000, 0x009bad85),
                ("tezro-tower-multi", 0x549bad85, 700_000_000, 0x009bad85),
                ("tezro-rack", 0x549bad85, 700_000_000, 0x009bad85),
                ("onyx4-host-serial-compat", 0x569dad85, 1_000_000_000,
                 0x009dad85)):
            with self.subTest(profile=name):
                model = _resolved(name)
                firmware = model["machine"]["firmware_config"]
                self.assertEqual(firmware["processor_firmware_word"], word)
                self.assertEqual(firmware["machine_type"], 0)

                topology = compile_model(model)

                self.assertEqual(topology["ip35_firmware"]["cpu_mode_word"],
                                 word)
                self.assertEqual(topology["ip35_firmware"]["machine_type"], 0)
                self.assertEqual(
                    topology["controllers"][0]["l1"]["cpu_context"], {
                        "cpu_mode_word": word,
                        "cpu_clock_hz": cpu_hz,
                        "hub_clock_hz": 200_000_000,
                    })
                emitted = json.loads(render_catalogue_json([model]))[
                    "topologies"][0]
                self.assertEqual(emitted["ip35-firmware"]["cpu-mode-word"],
                                 word)
                self.assertEqual(emitted["ip35-firmware"]["machine-type"], 0)
                self.assertEqual(
                    emitted["controllers"][0]["l1"]["cpu-context"], {
                        "cpu-mode-word": word,
                        "cpu-clock-hz": cpu_hz,
                        "hub-clock-hz": 200_000_000,
                    })
                self.assertEqual(
                    model["machine"]["firmware_config"][
                        "processor_reset_mode"], reset)

    def test_full_word_must_match_the_fitted_reset_mode(self):
        model = _resolved("origin350")
        model["machine"]["firmware_config"][
            "processor_firmware_word"] = 0x569cad84

        with self.assertRaisesRegex(ValueError, "low 25 bits"):
            compile_model(model)

    def test_full_word_cannot_opt_in_a_non_ip53_machine(self):
        model = _resolved("origin200")
        model["machine"]["firmware_config"][
            "processor_firmware_word"] = 0x569cad85

        with self.assertRaisesRegex(ValueError, "requires an IP53"):
            compile_model(model)


class BoardSelectionTests(unittest.TestCase):
    def model_with_bedrock_board(self, hardware_kind=None):
        model = _resolved("origin300")
        board = {
            "id": "selected-board",
            "name": "Selected board",
            "kind": "board",
            "provenance": "derived",
        }
        if hardware_kind is not None:
            board["hardware_kind"] = hardware_kind
        model["components"].append(board)
        bedrock = next(item for item in model["components"]
                       if item["id"] == "bedrock")
        bedrock["parent"] = board["id"]
        return model

    def test_nearest_board_identity_selects_ip53_without_changing_asic(self):
        model = PicIo9DescriptorTests().model_with_pic_io9(crossbar=True)
        model["components"].append({
            "id": "selected-board", "name": "Selected board", "kind": "board",
            "hardware_kind": "ip53", "provenance": "derived",
        })
        next(item for item in model["components"]
             if item["id"] == "bedrock")["parent"] = "selected-board"
        node = next(item for item in model["components"]
                    if item["id"] == "bedrock")
        node["identity"].update({
            "clock_mhz": 200, "ni_revision": 2, "widget_revision": 2,
        })

        compiled = compile_model(model)["nodes"][0]
        exported = json.loads(render_catalogue_json([model]))[
            "topologies"][0]["nodes"][0]

        self.assertEqual(compiled["board_kind"], "ip53")
        self.assertEqual(compiled["asic_kind"], "BEDROCK")
        self.assertEqual(exported["board-kind"], "ip53")
        self.assertEqual(exported["asic-kind"], "bedrock")

    def test_explicit_bedrock_board_requires_supported_identity(self):
        cases = ((None, "no hardware_kind"),
                 ("octane", "unsupported hardware_kind 'octane'"))
        for hardware_kind, message in cases:
            with self.subTest(hardware_kind=hardware_kind):
                with self.assertRaisesRegex(CompileError, message):
                    compile_model(self.model_with_bedrock_board(hardware_kind))

    def test_ip45_is_compiled_but_omitted_from_exports(self):
        model = self.model_with_bedrock_board("ip45")
        compiled = compile_model(model)["nodes"][0]
        exported = json.loads(render_catalogue_json([model]))[
            "topologies"][0]["nodes"][0]

        self.assertEqual(compiled["board_kind"], "ip45")
        self.assertNotIn("board-kind", exported)

    def test_flat_origin300_and_fuel_exports_omit_board_kind(self):
        for name in ("origin300", "fuel"):
            with self.subTest(configuration=name):
                model = _resolved(name)
                entry = compile_model(model)
                topology = json.loads(render_catalogue_json([model]))[
                    "topologies"][0]

                if name == "origin300":
                    self.assertNotIn("board_kind", entry["nodes"][0])
                else:
                    self.assertEqual(entry["nodes"][0]["board_kind"], "ip34")
                self.assertNotIn("board-kind", topology["nodes"][0])


class PicIo9DescriptorTests(unittest.TestCase):
    """Synthetic graph coverage for PIC host and IO9 descriptor plumbing."""

    def model_with_pic_io9(self, *, crossbar=False):
        model = _resolved("origin300")
        model["composition"]["product"] = "synthetic-pic-io9"
        model["machine"]["name"] = "Synthetic PIC and IO9 fixture"
        model["machine"]["model_role"] = "emulation-target"
        model["components"].extend([
            {"id": "fixture-pic", "name": "PIC fixture",
             "kind": "pci-host-bridge", "hardware_kind": "pic",
             "pci_host_bridge": {"heads": 2}, "provenance": "derived"},
            {"id": "fixture-io9", "name": "IO9 fixture",
             "kind": "board", "hardware_kind": "io9",
             "parent": "bedrock", "provenance": "derived",
             "identity": {"part": "synthetic-io9-part"}},
            {"id": "fixture-ioc4", "name": "Synthetic IOC4 fixture",
             "kind": "io-controller", "hardware_kind": "ioc4",
             "parent": "fixture-io9", "provenance": "derived",
             "identity": {"pci_revision": 0x53},
             "ioc4": {"clock_hz": 33333333}},
        ])
        model["buses"].append({"id": "fixture-pic-pci0",
                               "name": "Synthetic PCI bus",
                               "kind": "pci", "master": "fixture-pic",
                               "provenance": "derived"})
        if crossbar:
            model["buses"].append({"id": "fixture-pic-xio",
                                   "name": "Synthetic PIC XIO bus",
                                   "kind": "xio", "master": "xbridge-crossbar",
                                   "provenance": "derived"})
            upstream = {"component": "fixture-pic", "bus": "fixture-pic-xio",
                        "role": "bridge-upstream", "widget": 13,
                        "controlled_by": "bedrock", "provenance": "derived"}
        else:
            upstream = {"component": "fixture-pic", "bus": "xio",
                        "role": "bridge-upstream", "provenance": "derived",
                        "xio": {"enumerated_widget": 13,
                                "discovery_widget": 13,
                                "decoded_widgets": [13]}}
        model["connections"].extend([
            upstream,
            {"component": "fixture-pic", "bus": "fixture-pic-pci0",
             "role": "bridge-downstream", "pci_head": 0,
             "provenance": "derived"},
            {"component": "fixture-ioc4", "bus": "fixture-pic-pci0",
             "role": "endpoint", "pci_device": 1, "pci_function": 0,
             "intx_pin": "inta", "pic_input": 0,
             "provenance": "derived"},
        ])
        return model

    def model_with_two_pic_empty_io9(self):
        model = self.model_with_pic_io9()
        components = {item["id"]: item for item in model["components"]}
        pic = components.pop("fixture-pic")
        pic["id"] = "pic-xio-f"
        pic["name"] = "PIC widget 15"
        model["components"].append({
            "id": "pic-xio-b", "name": "PIC widget 11",
            "kind": "pci-host-bridge", "hardware_kind": "pic",
            "pci_host_bridge": {"heads": 2}, "provenance": "derived",
        })
        for bus in model["buses"]:
            if bus.get("master") == "fixture-pic":
                bus["master"] = "pic-xio-f"
        for connection in model["connections"]:
            if connection["component"] == "fixture-pic":
                connection["component"] = "pic-xio-f"
                if connection.get("role") == "bridge-downstream":
                    connection["bus"] = "fixture-pic-f-pci0"
                if connection["role"] == "bridge-upstream":
                    connection["xio"].update({
                        "enumerated_widget": 15,
                        "discovery_widget": 15,
                        "decoded_widgets": [15],
                    })
            if connection["component"] == "fixture-ioc4":
                connection["bus"] = "fixture-pic-f-pci0"
        model["buses"].append({
            "id": "fixture-pic-b-pci0", "name": "PIC widget 11 PCI head 0",
            "kind": "pci", "master": "pic-xio-b", "provenance": "derived",
        })
        model["buses"].append({
            "id": "fixture-pic-f-pci0", "name": "PIC widget 15 PCI head 0",
            "kind": "pci", "master": "pic-xio-f", "provenance": "derived",
        })
        model["buses"] = [
            bus for bus in model["buses"]
            if bus["id"] != "fixture-pic-pci0"
        ]
        model["connections"] = [
            connection for connection in model["connections"]
            if connection["component"] != "fixture-ioc4"
        ]
        model["components"] = [
            component for component in model["components"]
            if component["id"] != "fixture-ioc4"
        ]
        model["connections"].extend([
            {"component": "pic-xio-b", "bus": "xio",
             "role": "bridge-upstream", "provenance": "derived",
             "xio": {"enumerated_widget": 11, "discovery_widget": 11,
                     "decoded_widgets": [11]}},
            {"component": "pic-xio-b", "bus": "fixture-pic-b-pci0",
             "role": "bridge-downstream", "pci_head": 0,
             "provenance": "derived"},
            {"component": "fixture-io9", "bus": "fixture-pic-f-pci0",
             "role": "endpoint", "provenance": "derived"},
        ])
        return model

    def test_two_pic_io9_preserves_an_explicit_empty_endpoint_list(self):
        from jsonschema import Draft202012Validator

        model = self.model_with_two_pic_empty_io9()
        self.assertEqual(
            list(Draft202012Validator(schema(MACHINES)).iter_errors(model)), []
        )
        entry = compile_model(model)
        self.assertEqual([host["id"] for host in entry["pci_host_bridges"]],
                         ["pic-xio-b", "pic-xio-f"])
        board = next(item for item in entry["io_boards"]
                     if item["id"] == "fixture-io9")
        self.assertTrue(board["populated"])
        self.assertEqual(board["pci"], [])
        self.assertEqual((board["pci_host_index"], board["pci_head"]),
                         (1, 0))

        exported = json.loads(render_catalogue_json([model]))["topologies"][0]
        exported_board = next(item for item in exported["io-boards"]
                              if item["id"] == "fixture-io9")
        self.assertTrue(exported_board["populated"])
        self.assertEqual(exported_board["pci-functions"], [])
        self.assertEqual((exported_board["pci-host-index"],
                          exported_board["pci-head"]), (1, 0))

        absent = copy.deepcopy(model)
        absent["components"] = [
            item for item in absent["components"]
            if item["id"] != "fixture-io9"
        ]
        absent["connections"] = [
            item for item in absent["connections"]
            if item["component"] != "fixture-io9"
        ]
        self.assertFalse(any(
            item["board_kind"] == "IO9"
            for item in compile_model(absent)["io_boards"]
        ))

    def test_empty_io9_endpoint_list_requires_explicit_pci_bus_owner(self):
        model = self.model_with_two_pic_empty_io9()
        model["connections"] = [
            item for item in model["connections"]
            if item["component"] != "fixture-io9"
        ]

        with self.assertRaisesRegex(CompileError, "no authored PCI functions"):
            compile_model(model)

    def test_pic_profile_is_compiled_on_its_host_record(self):
        model = self.model_with_pic_io9()
        model["machine"]["emulation"]["pic_profiles"] = {
            "fixture-pic": "pic-rev3-bedrock",
        }

        entry = compile_model(model)

        self.assertEqual(entry["pci_host_bridges"], [{
            "id": "fixture-pic", "kind": "pic",
            "xio_attachment_index": len(entry["xio_attachments"]) - 1,
            "compatibility_profile": "pic-rev3-bedrock",
        }])

    def test_qtest_pic_profile_cannot_be_exported_as_a_product(self):
        model = self.model_with_pic_io9()
        model["machine"]["emulation"]["pic_profiles"] = {
            "fixture-pic": "pic-rev3-bedrock",
        }

        with self.assertRaisesRegex(CompileError, "qtest-only PIC profile"):
            render_catalogue_json([model])

    def test_pic_profile_cannot_disappear_without_an_emitted_host(self):
        model = self.model_with_pic_io9()
        model["machine"]["emulation"]["pic_profiles"] = {
            "fixture-pic": "pic-rev3-bedrock",
        }

        with mock.patch("sgi_model.model_compile._compile_pic_io_records",
                        return_value=([], [], [])):
            with self.assertRaisesRegex(
                    CompileError, "has no emitted PIC host descriptor"):
                compile_model(model)

    def test_io9_supports_isp12160_and_explicit_ioc4_serial_dma(self):
        from jsonschema import Draft202012Validator

        for enabled in (False, True):
            with self.subTest(serial_dma_experimental=enabled):
                model = self.io9_with_isp12160()
                ioc4 = next(item for item in model["components"]
                            if item["id"] == "fixture-ioc4")
                ioc4["ioc4"]["serial_dma_experimental"] = enabled

                errors = list(Draft202012Validator(schema(MACHINES)).iter_errors(model))
                self.assertEqual(errors, [])
                entry = compile_model(model)
                board = next(item for item in entry["io_boards"]
                             if item["id"] == "fixture-io9")
                functions = {item["kind"]: item for item in board["pci"]}
                self.assertEqual(functions["isp12160"]["revision"], 0x06)
                self.assertEqual(
                    functions["ioc4"]["serial_dma_experimental"], enabled)
                self.assertNotIn("serial_dma_experimental",
                                 functions["bcm5701"])

    def test_io9_isp12160_requires_its_supported_revision(self):
        model = self.io9_with_isp12160()
        isp = next(item for item in model["components"]
                   if item["id"] == "fixture-isp12160")
        isp["identity"].pop("pci_revision")

        with self.assertRaisesRegex(CompileError, "pci_revision"):
            compile_model(model)

    def test_io9_isp12160_rejects_a_duplicate_pic_route(self):
        model = self.io9_with_isp12160()
        route = next(item for item in model["connections"]
                     if item["component"] == "fixture-isp12160")
        route["pic_input"] = 3

        with self.assertRaisesRegex(CompileError, "multiple PCI functions route"):
            compile_model(model)

    def test_ip53_bedrock_clock_is_projected_as_checked_hz(self):
        model = self.model_with_pic_io9()
        components = {item["id"]: item for item in model["components"]}
        node = components["bedrock"]
        board = {"id": "fixture-ip53-node-board",
                 "name": "Synthetic IP53 node board", "kind": "board",
                 "hardware_kind": "ip53", "provenance": "derived"}
        model["components"].append(board)
        node["parent"] = board["id"]
        node["identity"].update({
            "clock_mhz": 200, "ni_revision": 2, "widget_revision": 2,
        })

        entry = compile_model(model)
        self.assertEqual(entry["nodes"][0]["identity"]["clock_hz"],
                         200_000_000)
        exported = json.loads(render_catalogue_json([model]))["topologies"][0]
        self.assertEqual(exported["nodes"][0]["identity"]["clock-hz"],
                         200_000_000)

        for identity, message in (
            ({"clock_mhz": True}, "clock_mhz"),
            ({"clock_mhz": (1 << 64) // 1_000_000 + 1}, "clock_mhz"),
            ({"clock_mhz": 200, "clock_hz": 201_000_000},
             "contradictory.*clock_hz"),
            ({"clock_hz": 200_000_000}, "requires identity.clock_mhz"),
        ):
            with self.subTest(identity=identity):
                invalid = self.model_with_pic_io9()
                components = {item["id"]: item
                              for item in invalid["components"]}
                node = components["bedrock"]
                board = {"id": "fixture-ip53-node-board",
                         "name": "Synthetic IP53 node board", "kind": "board",
                         "hardware_kind": "ip53", "provenance": "derived"}
                invalid["components"].append(board)
                node["parent"] = board["id"]
                node["identity"].pop("clock_mhz", None)
                node["identity"].pop("clock_hz", None)
                node["identity"].update({
                    "ni_revision": 2, "widget_revision": 2,
                })
                node["identity"].update(identity)
                with self.assertRaisesRegex(CompileError, message):
                    compile_model(invalid)

    def test_ip53_node_uses_independent_ni_and_widget_revisions(self):
        model = self.model_with_ip53_node()
        node = next(item for item in model["components"]
                    if item["id"] == "bedrock")
        node["identity"]["hardware_revision"] = 7

        identity = compile_model(model)["nodes"][0]["identity"]
        self.assertEqual(identity["ni_revision"], 1)
        self.assertEqual(identity["widget_revision"], 3)

    def test_ip53_node_revision_projection_rejects_missing_or_invalid_fields(self):
        for field, value in (("ni_revision", None),
                             ("widget_revision", None),
                             ("ni_revision", True),
                             ("widget_revision", "3"),
                             ("ni_revision", -1),
                             ("widget_revision", 16)):
            with self.subTest(field=field, value=value):
                model = self.model_with_ip53_node()
                identity = next(item for item in model["components"]
                                if item["id"] == "bedrock")["identity"]
                if value is None:
                    identity.pop(field)
                else:
                    identity[field] = value
                with self.assertRaises(CompileError):
                    compile_model(model)

    def test_ip53_node_revision_projection_has_no_hardware_revision_fallback(self):
        model = self.model_with_ip53_node()
        identity = next(item for item in model["components"]
                        if item["id"] == "bedrock")["identity"]
        identity.pop("ni_revision")
        identity.pop("widget_revision")
        identity["hardware_revision"] = 2

        with self.assertRaisesRegex(CompileError, "ni_revision"):
            compile_model(model)

    def test_bedrock_clock_identity_output_is_unchanged_for_other_boards(self):
        entry = compile_model(self.model_with_pic_io9())
        self.assertNotIn("clock_hz", entry["nodes"][0]["identity"])

    def test_pic_has_one_xio_host_record_and_io9_has_pci_reference(self):
        entry = compile_model(self.model_with_pic_io9())

        host = entry["pci_host_bridges"][0]
        board = next(board for board in entry["io_boards"]
                     if board["id"] == "fixture-io9")
        self.assertEqual(host, {
            "id": "fixture-pic", "kind": "pic",
            "xio_attachment_index": len(entry["xio_attachments"]) - 1,
        })
        self.assertEqual(board["pci_host_index"], 0)
        self.assertEqual(board["pci_head"], 0)
        self.assertTrue(board["populated"])
        self.assertEqual(board["pci"], [{
            "kind": "ioc4", "revision": 0x53,
            "pci_device": 1, "pci_function": 0,
            "intx_pin": "inta", "pic_input": 0,
            "clock_hz": 33333333,
        }])
        self.assertNotIn("slot_index", board)
        self.assertEqual(
            entry["xio_attachments"][host["xio_attachment_index"]], {
                "crossbar_index": -1, "widget": 13, "node_index": 0,
                "decoded_widget_mask": 1 << 13, "discovery_widget": 13,
            })
        exported = json.loads(
            render_catalogue_json([self.model_with_pic_io9()]))[
            "topologies"][0]
        self.assertEqual(exported["pci-host-bridges"], [{
            "id": "fixture-pic", "kind": "pic",
            "xio-attachment-index": host["xio_attachment_index"],
        }])
        from sgi_model.model_emit import _render_topology
        self.assertEqual(_render_topology(entry, "fixture-machine")
                         ["pci-host-bridges"][0], exported["pci-host-bridges"][0])
        exported_board = next(item for item in exported["io-boards"]
                              if item["id"] == "fixture-io9")
        self.assertEqual((exported_board["pci-host-index"],
                          exported_board["pci-head"]), (0, 0))
        self.assertNotIn("attachment", exported_board)
        self.assertTrue(exported_board["populated"])
        self.assertEqual(exported_board["pci-functions"], [{
            "kind": "ioc4", "revision": 0x53,
            "pci-device": 1, "pci-function": 0,
            "intx-pin": "inta", "pic-input": 0,
            "clock-hz": 33333333,
        }])

    def test_io9_clock_link_emits_a_ds1742_board_marker(self):
        model = self.model_with_ds1742_link()

        entry = compile_model(model)
        board = next(item for item in entry["io_boards"]
                     if item["id"] == "fixture-io9")
        self.assertTrue(board["ds1742"])
        exported = json.loads(render_catalogue_json([model]))["topologies"][0]
        exported_board = next(item for item in exported["io-boards"]
                              if item["id"] == "fixture-io9")
        self.assertTrue(exported_board["ds1742"])

    def test_io9_ds1742_year_base_defaults_to_zero_and_projects_when_set(self):
        model = self.model_with_ds1742_link()
        board = next(item for item in compile_model(model)["io_boards"]
                     if item["id"] == "fixture-io9")
        self.assertNotIn("ds1742_fresh_year_base", board)
        exported = json.loads(render_catalogue_json([model]))["topologies"][0]
        exported_board = next(item for item in exported["io-boards"]
                              if item["id"] == "fixture-io9")
        self.assertNotIn("ds1742-fresh-year-base", exported_board)

        source = next(item for item in model["components"]
                      if item["id"] == "fixture-ds1742")
        source["ds1742_fresh_year_base"] = 1968
        board = next(item for item in compile_model(model)["io_boards"]
                     if item["id"] == "fixture-io9")
        self.assertEqual(board["ds1742_fresh_year_base"], 1968)
        exported = json.loads(render_catalogue_json([model]))["topologies"][0]
        exported_board = next(item for item in exported["io-boards"]
                              if item["id"] == "fixture-io9")
        self.assertEqual(exported_board["ds1742-fresh-year-base"], 1968)

    def test_io9_ds1742_year_base_follows_nested_clock_ownership(self):
        model = self.model_with_ds1742_link()
        source = next(item for item in model["components"]
                      if item["id"] == "fixture-ds1742")
        model["components"].append({
            "id": "fixture-clock-carrier", "name": "RTC carrier",
            "kind": "subfunction", "parent": "fixture-io9",
            "provenance": "derived",
        })
        source["parent"] = "fixture-clock-carrier"
        source["ds1742_fresh_year_base"] = 1968

        board = next(item for item in compile_model(model)["io_boards"]
                     if item["id"] == "fixture-io9")
        self.assertEqual(board["ds1742_fresh_year_base"], 1968)
        exported = json.loads(render_catalogue_json([model]))["topologies"][0]
        exported_board = next(item for item in exported["io-boards"]
                              if item["id"] == "fixture-io9")
        self.assertEqual(exported_board["ds1742-fresh-year-base"], 1968)

    def test_io9_without_clock_link_omits_the_ds1742_board_marker(self):
        exported = json.loads(render_catalogue_json([
            self.model_with_pic_io9()
        ]))["topologies"][0]
        board = next(item for item in exported["io-boards"]
                     if item["id"] == "fixture-io9")

        self.assertNotIn("ds1742", board)

    def test_tezro_tower_emits_its_io9_timekeeper(self):
        topology = compile_model(_resolved("tezro-tower-multi"))
        board = next(item for item in topology["io_boards"]
                     if item["id"] == "io9")

        self.assertEqual([item["id"] for item in topology["pci_host_bridges"]],
                         ["pic-xio-b", "pic-xio-f"])
        self.assertNotIn("ds1742", board)
        self.assertEqual(board["system_timekeeper"]["kind"], "ds1744")
        self.assertEqual(board["system_timekeeper"]["year_base"], 1968)

    def test_io9_clock_link_schema_is_narrow_and_optional(self):
        from jsonschema import Draft202012Validator

        model = self.model_with_ds1742_link()
        self.assertEqual(
            list(Draft202012Validator(schema(MACHINES)).iter_errors(model)), []
        )
        model["links"][0]["kind"] = "physical-byte-bus"
        self.assertTrue(
            list(Draft202012Validator(schema(MACHINES)).iter_errors(model))
        )

    def test_linked_io9_boards_are_one_board_keyed_ds1742_resource(self):
        resources = _resources_for(
            {"generation": "SN1", "io_boards": [
                {"id": "io9-a-clock", "board_kind": "IO9", "ds1742": True,
                 "pci_host_index": 0, "pci": []},
                {"id": "io9-a", "board_kind": "IO9", "ds1742": True,
                 "pci_host_index": 1, "pci": []},
            ], "pci_host_bridges": [
                {"xio_attachment_index": 0},
                {"xio_attachment_index": 1},
            ], "xio_attachments": [
                {"node_index": 0}, {"node_index": 1},
            ], "controllers": []},
            {"requires_per_node_flash": False,
             "has_managed_boot_flash": False},
            {}, (), 2, (0, 1),
        )

        rtc_resources = [item for item in resources if item["kind"] == "ds1742"]
        self.assertEqual(rtc_resources, [{
            "kind": "ds1742", "scope": "board", "count": 2,
            "board-ids": ["io9-a", "io9-a-clock"],
            "argument": "-drive if=none,id=sgi-ds1742-data-BOARD",
            "part": "ds1742", "size": 2048, "clock-reference": True,
        }])

    def test_ds1742_resources_follow_the_offerings_surviving_nodes(self):
        topology = {
            "generation": "SN1",
            "io_boards": [
                {"id": "removed-board", "board_kind": "IO9",
                 "ds1742": True, "pci_host_index": 0, "pci": []},
                {"id": "retained-board", "board_kind": "IO9",
                 "ds1742": True, "pci_host_index": 1, "pci": []},
            ],
            "pci_host_bridges": [
                {"xio_attachment_index": 0},
                {"xio_attachment_index": 1},
            ],
            "xio_attachments": [
                {"node_index": 0}, {"node_index": 1},
            ],
            "controllers": [],
        }
        resources = _resources_for(
            topology,
            {"requires_per_node_flash": False,
             "has_managed_boot_flash": False},
            {}, (), 1, (1,),
        )

        rtc_resources = [item for item in resources
                         if item["kind"] == "ds1742"]
        self.assertEqual(rtc_resources[0]["count"], 1)
        self.assertEqual(rtc_resources[0]["board-ids"], ["retained-board"])

    def test_ds1742_resource_owners_must_have_unique_stable_ids(self):
        common = {
            "generation": "SN1",
            "pci_host_bridges": [
                {"xio_attachment_index": 0},
                {"xio_attachment_index": 1},
            ],
            "xio_attachments": [
                {"node_index": 0}, {"node_index": 1},
            ],
            "controllers": [],
        }
        harness = {"requires_per_node_flash": False,
                   "has_managed_boot_flash": False}
        cases = (
            ("duplicate IDs", ["io9-board", "io9-board"]),
            ("invalid stable ID", ["IO9-board", "io9-later"]),
        )
        for name, board_ids in cases:
            with self.subTest(name=name):
                topology = {
                    **common,
                    "io_boards": [
                        {"id": board_id, "board_kind": "IO9",
                         "ds1742": True, "pci_host_index": index,
                         "pci": []}
                        for index, board_id in enumerate(board_ids)
                    ],
                }
                with self.assertRaisesRegex(
                        ValueError,
                        "DS1742 resource owners must be unique stable board IDs"):
                    _resources_for(topology, harness, {}, (), 2, (0, 1))

    def test_offerings_filter_ds1742_owners_by_the_selected_node_row(self):
        model = _resolved("origin300-two-chassis")
        topology = compile_model(model)
        topology["io_boards"] = [
            {"id": "earlier-io9", "board_kind": "IO9", "ds1742": True,
             "pci_host_index": 0, "pci": []},
            {"id": "later-io9", "board_kind": "IO9", "ds1742": True,
             "pci_host_index": 1, "pci": []},
        ]
        topology["pci_host_bridges"] = [
            {"id": "earlier-pic", "kind": "PIC",
             "xio_attachment_index": 0},
            {"id": "later-pic", "kind": "PIC",
             "xio_attachment_index": 1},
        ]
        topology["xio_attachments"] = [
            {"node_index": 0}, {"node_index": 1},
        ]
        topology["node_populations"] = [{
            "nodes": [1], "routers": [], "cables": [], "xio_slots": [],
            "supported_cpu_populations": [[2]], "grade": "ESTABLISHED",
        }]
        product_models = {
            product: _resolved(model_name)
            for product, model_name in PRODUCT_MODELS.items()
        }

        offerings = _offerings_for(model, topology, product_models)

        self.assertEqual(len(offerings), 1)
        rtc_resources = [item for item in offerings[0]["resources"]
                         if item["kind"] == "ds1742"]
        self.assertEqual(rtc_resources[0]["count"], 1)
        self.assertEqual(rtc_resources[0]["board-ids"], ["later-io9"])

    def test_io9_clock_link_rejects_invalid_endpoints_and_duplicates(self):
        cases = (
            ("dangling", lambda model: model["links"][0].update(
                target="missing-ioc4"), "dangling"),
            ("unsupported link kind", lambda model: model["links"][0].update(
                kind="physical-byte-bus"), "unsupported kind"),
            ("wrong source", lambda model: model["links"][0].update(
                source="fixture-bcm5701"),
             "must source a supported timekeeper"),
            ("wrong kind", lambda model: model["links"][0].update(
                target="fixture-bcm5701"), "must target an IOC4"),
            ("duplicate", lambda model: model["links"].append(
                copy.deepcopy(model["links"][0])), "duplicate"),
        )
        for name, mutate, message in cases:
            with self.subTest(name=name):
                model = self.model_with_ds1742_link(with_bcm5701=True)
                mutate(model)
                with self.assertRaisesRegex(CompileError, message):
                    compile_model(model)

        model = self.model_with_ds1742_link()
        model["components"].append({
            "id": "fixture-ioc4-extra", "name": "Extra IOC4",
            "kind": "io-controller", "hardware_kind": "ioc4",
            "parent": "fixture-io9", "provenance": "derived",
        })
        with self.assertRaisesRegex(CompileError, "requires its one IOC4"):
            compile_model(model)

    def test_io9_clock_link_rejects_cross_board_endpoints(self):
        model = self.model_with_ds1742_link()
        model["components"].extend([
            {"id": "fixture-io9-second", "name": "Second IO9 fixture",
             "kind": "board", "hardware_kind": "io9",
             "parent": "bedrock", "provenance": "derived",
             "identity": {"part": "synthetic-io9-part-2"}},
            {"id": "fixture-ioc4-second", "name": "Second IOC4 fixture",
             "kind": "io-controller", "hardware_kind": "ioc4",
             "parent": "fixture-io9-second", "provenance": "derived",
             "identity": {"pci_revision": 0x53},
             "ioc4": {"clock_hz": 33333333}},
        ])
        model["buses"].append({"id": "fixture-pic-pci1",
                               "name": "Second synthetic PCI bus",
                               "kind": "pci", "master": "fixture-pic",
                               "provenance": "derived"})
        model["connections"].extend([
            {"component": "fixture-pic", "bus": "fixture-pic-pci1",
             "role": "bridge-downstream", "pci_head": 1,
             "provenance": "derived"},
            {"component": "fixture-ioc4-second", "bus": "fixture-pic-pci1",
             "role": "endpoint", "pci_device": 1, "pci_function": 0,
             "intx_pin": "inta", "pic_input": 1,
             "provenance": "derived"},
        ])
        model["links"][0]["target"] = "fixture-ioc4-second"

        with self.assertRaisesRegex(CompileError, "same IO9 board"):
            compile_model(model)

    def model_with_ds1742_link(self, *, with_bcm5701=False):
        model = self.model_with_pic_io9()
        model["components"].append({
            "id": "fixture-ds1742", "name": "Synthetic DS1742",
            "kind": "rtc", "hardware_kind": "ds1742",
            "parent": "fixture-io9", "provenance": "derived",
        })
        if with_bcm5701:
            model = self.io9_with_ioc4_and_bcm5701()
            model["components"].append({
                "id": "fixture-ds1742", "name": "Synthetic DS1742",
                "kind": "rtc", "hardware_kind": "ds1742",
                "parent": "fixture-io9", "provenance": "derived",
            })
        model["links"] = [{
            "kind": "ioc4-clock", "source": "fixture-ds1742",
            "target": "fixture-ioc4",
        }]
        return model

    def test_io9_functions_preserve_positions_routes_revisions_and_clock(self):
        model = self.model_with_pic_io9()
        model["components"] = [
            component for component in model["components"]
            if component["id"] != "fixture-ioc4"
        ]
        model["components"].extend([
            {"id": "fixture-ioc4", "name": "Synthetic IOC4",
             "kind": "io-controller", "hardware_kind": "ioc4",
             "parent": "fixture-io9", "provenance": "derived",
             "identity": {"pci_revision": 0x53},
             "ioc4": {"clock_hz": 33333333}},
            {"id": "fixture-bcm5701", "name": "Synthetic BCM5701",
             "kind": "bus-controller", "hardware_kind": "bcm5701",
             "parent": "fixture-io9", "provenance": "derived",
             "identity": {"pci_revision": 0}},
        ])
        model["connections"] = [
            connection for connection in model["connections"]
            if connection.get("component") != "fixture-ioc4"
        ]
        model["connections"].extend([
            {"component": "fixture-bcm5701", "bus": "fixture-pic-pci0",
             "role": "endpoint", "pci_device": 4,
             "intx_pin": "intd", "pic_input": 7,
             "provenance": "derived"},
            {"component": "fixture-ioc4", "bus": "fixture-pic-pci0",
             "role": "endpoint", "pci_device": 1, "pci_function": 2,
             "intx_pin": "inta", "pic_input": 3,
             "provenance": "derived"},
        ])

        from jsonschema import Draft202012Validator
        errors = list(Draft202012Validator(schema(MACHINES)).iter_errors(model))
        self.assertEqual(errors, [])
        entry = compile_model(model)
        board = next(item for item in entry["io_boards"]
                     if item["id"] == "fixture-io9")
        self.assertEqual(board["pci"], [
            {"kind": "ioc4", "revision": 0x53,
             "pci_device": 1, "pci_function": 2,
             "intx_pin": "inta", "pic_input": 3,
             "clock_hz": 33333333},
            {"kind": "bcm5701", "revision": 0,
             "pci_device": 4, "pci_function": 0,
             "intx_pin": "intd", "pic_input": 7},
        ])
        exported = json.loads(render_catalogue_json([model]))[
            "topologies"][0]
        output_board = next(item for item in exported["io-boards"]
                            if item["id"] == "fixture-io9")
        self.assertEqual(output_board["pci-functions"], [
            {"kind": "ioc4", "revision": 0x53,
             "pci-device": 1, "pci-function": 2,
             "intx-pin": "inta", "pic-input": 3,
             "clock-hz": 33333333},
            {"kind": "bcm5701", "revision": 0,
             "pci-device": 4, "pci-function": 0,
             "intx-pin": "intd", "pic-input": 7},
        ])

    def test_io9_rejects_device_zero_duplicate_devfn_and_bad_routes(self):
        mutations = (
            ("pci_device", 0, "device"),
            ("pci_device", 32, "device"),
            ("pci_function", 8, "function"),
            ("intx_pin", "inte", "INTx"),
            ("pic_input", 8, "PIC input"),
        )
        for field, value, message in mutations:
            with self.subTest(field=field, value=value):
                model = self.io9_with_ioc4_and_bcm5701()
                connection = next(item for item in model["connections"]
                                  if item["component"] == "fixture-ioc4")
                connection[field] = value
                with self.assertRaisesRegex(CompileError, message):
                    compile_model(model)

        model = self.io9_with_ioc4_and_bcm5701()
        model["connections"][-1]["pci_device"] = 1
        model["connections"][-1]["pci_function"] = 2
        with self.assertRaisesRegex(CompileError, "duplicate PCI devfn"):
            compile_model(model)

    def test_io9_requires_explicit_placement_route_and_clock(self):
        mutations = (
            ("pci_device", None, "pci_device"),
            ("intx_pin", None, "INTx pin"),
            ("pic_input", None, "PIC input"),
        )
        for field, value, message in mutations:
            with self.subTest(field=field):
                model = self.io9_with_ioc4_and_bcm5701()
                connection = next(item for item in model["connections"]
                                  if item["component"] == "fixture-ioc4")
                if value is None:
                    connection.pop(field)
                else:
                    connection[field] = value
                with self.assertRaisesRegex(CompileError, message):
                    compile_model(model)

        model = self.io9_with_ioc4_and_bcm5701()
        ioc4 = next(item for item in model["components"]
                    if item["id"] == "fixture-ioc4")
        ioc4["ioc4"].pop("clock_hz")
        with self.assertRaisesRegex(CompileError, "clock_hz"):
            compile_model(model)

        model = self.io9_with_ioc4_and_bcm5701()
        ioc4 = next(item for item in model["components"]
                    if item["id"] == "fixture-ioc4")
        ioc4.pop("ioc4")
        with self.assertRaisesRegex(CompileError, "clock_hz"):
            compile_model(model)

    def test_io9_revision_override_and_unsupported_revision(self):
        model = self.io9_with_ioc4_and_bcm5701()
        model["machine"]["emulation"]["pci_function_revisions"] = {
            "fixture-ioc4": {
                "revision": 0x53, "provenance": "derived",
                "sources": ["synthetic compiler fixture"],
                "notes": "Synthetic revision override fixture.",
            }
        }
        from jsonschema import Draft202012Validator
        errors = list(Draft202012Validator(schema(MACHINES)).iter_errors(model))
        self.assertEqual(errors, [])
        entry = compile_model(model)
        board = next(item for item in entry["io_boards"]
                     if item["id"] == "fixture-io9")
        self.assertEqual(board["pci"][0]["revision"], 0x53)

        model = self.io9_with_ioc4_and_bcm5701()
        next(item for item in model["components"]
             if item["id"] == "fixture-ioc4")["identity"]["pci_revision"] = 0x52
        with self.assertRaisesRegex(CompileError, "unsupported ioc4 revision"):
            compile_model(model)

    def test_io9_rejects_clock_values_and_shared_pic_inputs(self):
        for clock_hz in (0, 1, 4294967296):
            with self.subTest(clock_hz=clock_hz):
                model = self.io9_with_ioc4_and_bcm5701()
                next(item for item in model["components"]
                     if item["id"] == "fixture-ioc4")["ioc4"][
                         "clock_hz"] = clock_hz
                with self.assertRaisesRegex(CompileError, "clock_hz"):
                    compile_model(model)

        model = self.io9_with_ioc4_and_bcm5701()
        next(item for item in model["connections"]
             if item["component"] == "fixture-bcm5701")["pic_input"] = 3
        with self.assertRaisesRegex(CompileError, "multiple PCI functions"):
            compile_model(model)

    def io9_with_ioc4_and_bcm5701(self):
        model = self.model_with_pic_io9()
        model["components"] = [
            component for component in model["components"]
            if component["id"] != "fixture-ioc4"
        ]
        model["components"].extend([
            {"id": "fixture-ioc4", "name": "Synthetic IOC4",
             "kind": "io-controller", "hardware_kind": "ioc4",
             "parent": "fixture-io9", "provenance": "derived",
             "identity": {"pci_revision": 0x53},
             "ioc4": {"clock_hz": 33333333}},
            {"id": "fixture-bcm5701", "name": "Synthetic BCM5701",
             "kind": "bus-controller", "hardware_kind": "bcm5701",
             "parent": "fixture-io9", "provenance": "derived",
             "identity": {"pci_revision": 0}},
        ])
        model["connections"] = [
            connection for connection in model["connections"]
            if connection.get("component") != "fixture-ioc4"
        ]
        model["connections"].extend([
            {"component": "fixture-ioc4", "bus": "fixture-pic-pci0",
             "role": "endpoint", "pci_device": 1, "pci_function": 2,
             "intx_pin": "inta", "pic_input": 3,
             "provenance": "derived"},
            {"component": "fixture-bcm5701", "bus": "fixture-pic-pci0",
             "role": "endpoint", "pci_device": 4,
             "intx_pin": "intd", "pic_input": 7,
             "provenance": "derived"},
        ])
        return model

    def io9_with_isp12160(self):
        model = self.io9_with_ioc4_and_bcm5701()
        model["components"].append({
            "id": "fixture-isp12160", "name": "Synthetic ISP12160 fixture",
            "kind": "storage-hba", "hardware_kind": "isp12160",
            "parent": "fixture-io9", "provenance": "derived",
            "identity": {"pci_revision": 0x06},
        })
        model["connections"].append({
            "component": "fixture-isp12160", "bus": "fixture-pic-pci0",
            "role": "endpoint", "pci_device": 2, "pci_function": 1,
            "intx_pin": "intb", "pic_input": 1,
            "provenance": "derived",
        })
        return model

    def model_with_ip53_node(self, *, pic=True, crossbar=True):
        model = (self.model_with_pic_io9(crossbar=crossbar) if pic
                 else _resolved("origin300"))
        components = {item["id"]: item for item in model["components"]}
        board = {"id": "fixture-ip53-node-board",
                 "name": "Synthetic IP53 node board", "kind": "board",
                 "hardware_kind": "ip53", "provenance": "derived"}
        model["components"].append(board)
        node = components["bedrock"]
        node["parent"] = board["id"]
        node.setdefault("identity", {}).update({
            "clock_mhz": 200, "ni_revision": 1, "widget_revision": 3,
        })
        return model

    def test_ip53_node_uses_authored_pic_crossbar_path(self):
        entry = compile_model(self.model_with_ip53_node())
        host = entry["pci_host_bridges"][0]
        attachment = entry["xio_attachments"][host["xio_attachment_index"]]
        self.assertEqual(attachment["crossbar_index"], 0)
        self.assertEqual(attachment["widget"], 13)
        self.assertEqual(attachment["node_index"], 0)

    def test_ip53_dispatch_refuses_mixed_node_boards(self):
        model = _resolved("origin300-two-chassis")
        model["components"].append({
            "id": "fixture-ip53-node-board",
            "name": "Synthetic IP53 node board", "kind": "board",
            "hardware_kind": "ip53", "provenance": "derived",
        })
        node = next(item for item in model["components"]
                    if item["id"] == "master-bedrock")
        node["parent"] = "fixture-ip53-node-board"
        node["identity"].update({
            "clock_mhz": 200, "ni_revision": 2, "widget_revision": 2,
        })

        with self.assertRaisesRegex(CompileError,
                                    "mixed IP53 and non-IP53 SN1 nodes"):
            compile_model(model)

    def test_ip53_node_requires_pic_attached_to_that_node(self):
        model = self.model_with_ip53_node()

        def foreign_pic(_model, _graph, _modules, first_attachment,
                        _ds1742_boards):
            return ([{"id": "fixture-pic", "kind": "pic",
                      "xio_attachment_index": first_attachment}], [],
                    [{"crossbar_index": 0, "widget": 13,
                      "node_index": 1}])

        with mock.patch("sgi_model.model_compile._compile_pic_io_records",
                        side_effect=foreign_pic):
            with self.assertRaisesRegex(
                    CompileError,
                    "IP53 Bedrock node 0 has no authored PIC attachment"):
                compile_model(model)

    def test_ip53_node_requires_an_authored_pic_host(self):
        with self.assertRaisesRegex(CompileError,
                                    "IP53 Bedrock nodes require an authored PIC"):
            compile_model(self.model_with_ip53_node(pic=False))

    def test_product_name_does_not_select_ip53_node_path(self):
        model = self.model_with_pic_io9(crossbar=True)
        baseline = compile_model(model)
        model["composition"]["product"] = "origin350"

        entry = compile_model(model)
        for field in ("io_boards", "xio_attachments", "xio_slots",
                      "pci_host_bridges"):
            with self.subTest(field=field):
                self.assertEqual(entry[field], baseline[field])

    def test_pic_crossbar_route_is_preserved_as_authored(self):
        entry = compile_model(self.model_with_pic_io9(crossbar=True))
        host = entry["pci_host_bridges"][0]
        attachment = entry["xio_attachments"][host["xio_attachment_index"]]
        self.assertEqual(attachment["crossbar_index"], 0)
        self.assertEqual(attachment["widget"], 13)
        self.assertEqual(attachment["node_index"], 0)

    def test_pic_downstream_bus_requires_authored_head(self):
        model = self.model_with_pic_io9()
        next(item for item in model["connections"]
             if item.get("bus") == "fixture-pic-pci0"
             and item.get("role") == "bridge-downstream").pop("pci_head")
        with self.assertRaisesRegex(CompileError, "pci_head"):
            compile_model(model)

    def test_pic_last_declared_downstream_head_is_valid(self):
        model = self.model_with_pic_io9()
        next(item for item in model["connections"]
             if item.get("bus") == "fixture-pic-pci0"
             and item.get("role") == "bridge-downstream")["pci_head"] = 1
        entry = compile_model(model)
        board = next(item for item in entry["io_boards"]
                     if item["id"] == "fixture-io9")
        self.assertEqual(board["pci_head"], 1)

    def test_pic_downstream_head_must_fit_capacity(self):
        model = self.model_with_pic_io9()
        next(item for item in model["connections"]
             if item.get("bus") == "fixture-pic-pci0"
             and item.get("role") == "bridge-downstream")["pci_head"] = 2
        with self.assertRaisesRegex(CompileError, "head"):
            compile_model(model)

    def test_pic_declared_capacity_must_match_two_head_contract(self):
        model = self.model_with_pic_io9()
        next(item for item in model["components"]
             if item["id"] == "fixture-pic")["pci_host_bridge"]["heads"] = 3
        next(item for item in model["connections"]
             if item.get("bus") == "fixture-pic-pci0"
             and item.get("role") == "bridge-downstream")["pci_head"] = 2
        with self.assertRaisesRegex(CompileError, "two-head capacity"):
            compile_model(model)

    def test_pci_head_requires_a_pci_downstream_bus(self):
        model = self.model_with_pic_io9()
        next(item for item in model["buses"]
             if item["id"] == "fixture-pic-pci0")["kind"] = "xio"
        with self.assertRaisesRegex(CompileError, "downstream PCI bus"):
            compile_model(model)

    def test_pci_head_requires_a_pic_host_even_without_io9(self):
        model = self.model_with_pic_io9()
        model["components"] = [
            item for item in model["components"]
            if item["id"] not in {"fixture-io9", "fixture-ioc4"}
        ]
        model["connections"] = [item for item in model["connections"]
                                if item.get("component") != "fixture-ioc4"]
        next(item for item in model["components"]
             if item["id"] == "fixture-pic")["hardware_kind"] = (
                 "unsupported-host")
        with self.assertRaisesRegex(CompileError, "explicitly described PIC"):
            compile_model(model)

    def test_io9_requires_a_board_part_identity(self):
        model = self.model_with_pic_io9()
        next(item for item in model["components"]
             if item["id"] == "fixture-io9")["identity"].pop("part")
        with self.assertRaisesRegex(CompileError, "identity.part"):
            compile_model(model)

    def test_io9_requires_a_fitted_pci_endpoint(self):
        model = self.model_with_pic_io9()
        model["connections"] = [item for item in model["connections"]
                                if not (item.get("component") == "fixture-ioc4"
                                        and item.get("role") == "endpoint")]
        with self.assertRaisesRegex(CompileError, "no authored PCI functions"):
            compile_model(model)

    def test_pci_head_is_only_valid_on_downstream_connections(self):
        model = self.model_with_pic_io9()
        connection = next(item for item in model["connections"]
                          if item.get("bus") == "fixture-pic-pci0"
                          and item.get("role") == "bridge-downstream")
        connection["role"] = "endpoint"
        with self.assertRaisesRegex(CompileError, "bridge-downstream"):
            compile_model(model)

        from jsonschema import Draft202012Validator
        errors = list(Draft202012Validator(schema(MACHINES)).iter_errors(model))
        self.assertTrue(errors)

    def test_negative_pic_head_is_rejected(self):
        model = self.model_with_pic_io9()
        next(item for item in model["connections"]
             if item.get("bus") == "fixture-pic-pci0"
             and item.get("role") == "bridge-downstream")["pci_head"] = -1
        with self.assertRaisesRegex(CompileError, "pci_head"):
            compile_model(model)

    def test_duplicate_pic_downstream_head_is_rejected(self):
        model = self.model_with_pic_io9()
        model["buses"].append({"id": "fixture-pic-pci1",
                               "name": "Second synthetic PCI bus",
                               "kind": "pci", "master": "fixture-pic",
                               "provenance": "derived"})
        model["connections"].append({
            "component": "fixture-pic", "bus": "fixture-pic-pci1",
            "role": "bridge-downstream", "pci_head": 0,
            "provenance": "derived"})
        with self.assertRaisesRegex(
                CompileError, "head.*more than once|duplicate.*head"):
            compile_model(model)

    def test_io9_functions_must_resolve_through_one_downstream_bus(self):
        model = self.model_with_pic_io9()
        model["buses"].append({"id": "fixture-pic-pci1",
                               "name": "Second synthetic PCI bus",
                               "kind": "pci", "master": "fixture-pic",
                               "provenance": "derived"})
        model["connections"].extend([
            {"component": "fixture-pic", "bus": "fixture-pic-pci1",
             "role": "bridge-downstream", "pci_head": 1,
             "provenance": "derived"},
            {"component": "fixture-ioc4", "bus": "fixture-pic-pci1",
             "role": "endpoint", "pci_device": 2, "pci_function": 0,
             "intx_pin": "intb", "pic_input": 1,
             "provenance": "derived"},
        ])
        with self.assertRaisesRegex(
                CompileError, "one downstream bus|same.*bus"):
            compile_model(model)

    def test_duplicate_io9_board_host_head_reference_is_rejected(self):
        model = self.model_with_pic_io9()
        model["components"].extend([
            {"id": "fixture-io9-second", "name": "Second IO9 fixture",
             "kind": "board", "hardware_kind": "io9", "parent": "bedrock",
             "provenance": "derived"},
            {"id": "fixture-ioc4-second", "name": "Second IOC4 function",
             "kind": "io-controller", "hardware_kind": "ioc4",
             "parent": "fixture-io9-second", "provenance": "derived",
             "identity": {"pci_revision": 0x53},
             "ioc4": {"clock_hz": 33333333}},
        ])
        model["connections"].append({
            "component": "fixture-ioc4-second", "bus": "fixture-pic-pci0",
            "role": "endpoint", "pci_device": 2, "pci_function": 0,
            "intx_pin": "intb", "pic_input": 1,
            "provenance": "derived"})
        with self.assertRaisesRegex(CompileError, "multiple IO9 board records"):
            compile_model(model)

    def test_io9_cannot_reference_unsupported_host_kind(self):
        model = self.model_with_pic_io9()
        next(item for item in model["components"]
             if item["id"] == "fixture-pic")["hardware_kind"] = (
                 "unsupported-host")
        with self.assertRaisesRegex(CompileError, "explicitly described PIC"):
            compile_model(model)


class RecordShapeTests(unittest.TestCase):
    """Compile shape per record type, read off real resolved fixtures."""

    def test_synthetic_expansion_pending_selection(self):
        model = _resolved(
            "origin300-pci-expansion")
        raw = model["machine"]["emulation"]["io_assemblies"][0]
        raw["pending_register_experiment"] = True
        descriptor = compile_model(model)["io_assemblies"][0]
        self.assertTrue(descriptor["pending_register_experiment"])
        raw["pending_register_experiment"] = False
        with self.assertRaisesRegex(
                CompileError, "pending-register experiment"):
            compile_model(model)
        raw["pending_register_experiment"] = True
        del raw["unconnected_uart"]
        with self.assertRaisesRegex(
                CompileError, "pending-register experiment"):
            compile_model(model)

    def test_synthetic_expansion_uart_selection(self):
        model = _resolved(
            "origin300-pci-expansion")
        raw = model["machine"]["emulation"]["io_assemblies"][0]
        raw["unconnected_uart"] = True
        descriptor = compile_model(model)["io_assemblies"][0]
        self.assertTrue(descriptor["unconnected_uart"])
        raw["unconnected_uart"] = False
        with self.assertRaisesRegex(CompileError, "synthetic UART"):
            compile_model(model)
        raw["unconnected_uart"] = True
        del raw["configuration_storage"]
        with self.assertRaisesRegex(CompileError, "synthetic UART"):
            compile_model(model)

    def test_io_only_bedrock_is_a_module_owned_fabric_peer(self):
        model = _resolved("origin300")
        model["components"].extend([
            {"id": "io-enclosure", "name": "Test I/O enclosure", "kind": "module"},
            {"id": "io-bedrock", "name": "Test I/O-only Bedrock",
             "kind": "bus-controller", "hardware_kind": "bedrock",
             "parent": "io-enclosure"},
        ])
        next(bus for bus in model["buses"] if bus["id"] == "numalink")[
            "link"] = {"kind": "cable"}
        model["connections"].append({
            "component": "io-bedrock", "bus": "numalink", "role": "endpoint",
        })
        model["components"].append({
            "id": "fabric-connector", "kind": "connector",
        })
        model["connections"].append({
            "component": "fabric-connector", "bus": "numalink", "role": "connector",
        })
        model["composition"].setdefault("connector_population", []).append({
            "component": "fabric-connector", "occupancy": "occupied",
            "occupant": "io-enclosure",
        })

        graph = index_graph(model)

        self.assertEqual([node["id"] for node in graph["nodes"]], ["bedrock"])
        self.assertEqual([(peer["id"], peer["module_index"])
                          for peer in graph["io_peers"]], [("io-bedrock", 1)])
        self.assertEqual(graph["links"], [{
            "name": "numalink", "kind": "cable", "ends": [
                {"kind": "NODE", "index": 0, "port": 0},
                {"kind": "IO_PEER", "index": 0, "port": 0},
            ],
        }])

    def test_io_peer_without_identity_is_not_compiled_as_a_compute_node(self):
        model = _resolved("origin300")
        model["components"].extend([
            {"id": "io-enclosure", "name": "Test I/O enclosure", "kind": "module"},
            {"id": "io-bedrock", "name": "Test I/O-only Bedrock",
             "kind": "bus-controller", "hardware_kind": "bedrock",
             "parent": "io-enclosure"},
        ])
        with self.assertRaisesRegex(CompileError, "I/O peer 'io-bedrock'.*identity"):
            compile_model(model)

    def test_explicit_synthetic_io_peer_exports_no_cpu_memory_or_flash(self):
        model = _resolved("origin300")
        model["components"].extend([
            {"id": "io-enclosure", "name": "Test I/O enclosure", "kind": "module"},
            {"id": "io-bedrock", "name": "Test I/O-only Bedrock",
             "kind": "bus-controller", "hardware_kind": "bedrock",
             "parent": "io-enclosure", "identity": {"hardware_revision": 1},
             "datasheet_contracts": ["qemu:docs/system/mips/sgi/hardware/hub.rst"]},
        ])
        model["machine"]["emulation"].setdefault("synthetic_identities", []).append({
            "component": "io-bedrock", "kind": "bedrock", "serial": "0x51454d554e05",
        })
        entry = compile_model(model)
        self.assertEqual(len(entry["nodes"]), 1)
        self.assertEqual(entry["io_peers"], [{
            "id": "io-bedrock", "module_index": 1, "slot": 0,
            "asic_kind": "BEDROCK", "identity": {
                "ni_revision": 1, "widget_revision": 1, "serial": 0x51454d554e05,
                "i2c_controller": False, "system_controller": False,
            },
        }])
        self.assertEqual(entry["modules"][1]["nodes"], [])
        self.assertEqual(entry["modules"][1]["io_peers"], [0])
        catalogue = json.loads(render_catalogue_json([model]))
        peer = catalogue["topologies"][0]["io-peers"][0]
        self.assertEqual(set(peer), {"id", "module-index", "slot", "asic-kind", "identity"})
        self.assertEqual(peer["asic-kind"], "bedrock")
        self.assertEqual(catalogue["topologies"][0]["modules"][1]["io-peers"], [0])
        for missing in ("identity", "datasheet_contracts"):
            with self.subTest(missing=missing):
                changed = copy.deepcopy(model)
                del changed["components"][-1][missing]
                with self.assertRaises(CompileError):
                    compile_model(changed)
        changed = copy.deepcopy(model)
        changed["machine"]["emulation"]["synthetic_identities"] = [
            item for item in changed["machine"]["emulation"]["synthetic_identities"]
            if item["component"] != "io-bedrock"]
        with self.assertRaisesRegex(CompileError, "explicit synthetic identity"):
            compile_model(changed)
        for kind in ("cpu", "memory", "flash"):
            with self.subTest(kind=kind):
                changed = copy.deepcopy(model)
                changed["components"].append({
                    "id": "forbidden", "kind": kind, "parent": "io-enclosure",
                })
                with self.assertRaisesRegex(CompileError, "cannot own " + kind):
                    compile_model(changed)

    def _synthetic_io_peer_model(self, name="origin300"):
        model = _resolved(name)
        model["components"].extend([
            {"id": "io-enclosure", "name": "Synthetic I/O module", "kind": "module"},
            {"id": "io-bedrock", "name": "Synthetic I/O-only Bedrock",
             "kind": "bus-controller", "hardware_kind": "bedrock",
             "parent": "io-enclosure", "identity": {"hardware_revision": 1},
             "datasheet_contracts": ["qemu:docs/system/mips/sgi/hardware/hub.rst"]},
        ])
        model["machine"]["emulation"].setdefault("synthetic_identities", []).append({
            "component": "io-bedrock", "kind": "bedrock", "serial": "0x51454d554e05",
        })
        return model

    def _expansion_descriptor_model(self):
        model = self._synthetic_io_peer_model()
        model["components"].append({
            "id": "io-l1", "kind": "system-controller", "parent": "io-enclosure",
            "system_controller": {"kind": "l1"},
        })
        model["machine"]["emulation"]["io_assemblies"] = [{
            "version": 1, "kind": "origin300-pci-expansion", "peer": "io-bedrock",
            "module": "io-enclosure", "controller": "io-l1", "placement": 1,
            "initial_node_number": 0, "bedrock_revision": 2, "xbridge_revision": 2,
            "head_ports": [8, 9, 12, 13], "controller_profile": "pci-expansion",
            "bic_identity": 0xc000,
        }]
        return model

    def test_expansion_experiment_owns_its_internal_xio_links(self):
        model = _resolved("origin300-pci-expansion")
        compiled = compile_model(model)
        self.assertEqual(len(compiled["nodes"]), 1)
        self.assertEqual(len(compiled["io_peers"]), 1)
        self.assertEqual(len(compiled["controllers"]), 2)
        self.assertEqual(compiled["io_assemblies"][0]["initial_node_number"], 0)
        self.assertEqual(compiled["io_assemblies"][0]["head_ports"], [8, 9, 12, 13])
        self.assertEqual(len(compiled["xio_attachments"]), 2)

    def test_expansion_internal_xio_rejects_foreign_module_endpoint(self):
        model = self._expansion_descriptor_model()
        model["buses"].append({
            "id": "assembly-xio", "kind": "xio", "master": "io-bedrock",
        })
        compute = next(c["id"] for c in model["components"]
                       if c.get("kind") == "node-asic")
        model["connections"].append({
            "component": compute, "bus": "assembly-xio", "role": "endpoint",
        })
        with self.assertRaisesRegex(CompileError, "leaves its module"):
            index_graph(model)

    def test_expansion_descriptor_retains_reset_zero_separately_from_placement(self):
        model = self._expansion_descriptor_model()
        descriptor = compile_model(model)["io_assemblies"][0]
        self.assertEqual(descriptor["kind"], "ORIGIN300_PCI_EXPANSION")
        self.assertEqual(descriptor["peer_index"], 0)
        self.assertEqual(descriptor["module_index"], 1)
        self.assertEqual(descriptor["initial_node_number"], 0)
        self.assertEqual(descriptor["placement"], 1)
        self.assertNotIn("initial_latch", descriptor)
        self.assertNotIn("intx_map", descriptor)

    def test_expansion_configuration_storage_requires_power_selection(self):
        model = self._expansion_descriptor_model()
        raw = model["machine"]["emulation"]["io_assemblies"][0]
        self.assertNotIn("configuration_storage",
                         compile_model(model)["io_assemblies"][0])
        raw["configuration_storage"] = "bedrock-flash-experiment"
        with self.assertRaisesRegex(CompileError, "configuration storage"):
            compile_model(model)
        raw.update(main_power="functional-experiment", initial_main_power=True)
        descriptor = compile_model(model)["io_assemblies"][0]
        self.assertEqual(descriptor["configuration_storage"],
                         "bedrock-flash-experiment")
        emitted = json.loads(render_catalogue_json([model]))["topologies"][0]
        self.assertEqual(emitted["io-assemblies"][0]["configuration-storage"],
                         "bedrock-flash-experiment")
        raw["configuration_storage"] = "physical"
        with self.assertRaisesRegex(CompileError, "configuration storage"):
            compile_model(model)

    def test_expansion_main_power_requires_explicit_selection_and_state(self):
        model = self._expansion_descriptor_model()
        raw = model["machine"]["emulation"]["io_assemblies"][0]
        self.assertNotIn("main_power", compile_model(model)["io_assemblies"][0])
        for initial in (True, False):
            raw.update(main_power="functional-experiment",
                       initial_main_power=initial)
            descriptor = compile_model(model)["io_assemblies"][0]
            self.assertEqual(descriptor["main_power"], "functional-experiment")
            self.assertIs(descriptor["initial_main_power"], initial)
            emitted = json.loads(
                render_catalogue_json([model]))["topologies"][0]
            self.assertIs(emitted["io-assemblies"][0]["initial-main-power"],
                          initial)
        for fields in ({"main_power": "functional-experiment"},
                       {"initial_main_power": True},
                       {"main_power": "physical", "initial_main_power": True},
                       {"main_power": "functional-experiment",
                        "initial_main_power": 1}):
            raw.pop("main_power", None)
            raw.pop("initial_main_power", None)
            raw.update(fields)
            with self.assertRaisesRegex(CompileError, "main power"):
                compile_model(model)

    def test_power_command_requires_selected_power_and_compute_owner(self):
        model = _resolved("origin300-pci-expansion")
        raw = model["machine"]["emulation"]["io_assemblies"][0]
        # Storage and the synthetic registers need main power themselves,
        # so drop them to reach the power command's own check.
        for key in ("configuration_storage", "unconnected_uart",
                    "pending_register_experiment"):
            raw.pop(key, None)
        raw["power_command_issuer"] = "compute-l1"
        descriptor = compile_model(model)["io_assemblies"][0]
        self.assertEqual(descriptor["power_command_issuer"], 0)
        emitted = json.loads(render_catalogue_json([model]))["topologies"][0]
        self.assertEqual(emitted["io-assemblies"][0]["power-command-issuer"], 0)
        for issuer in ("missing", "expansion-l1", 0):
            raw["power_command_issuer"] = issuer
            with self.assertRaisesRegex(CompileError, "power command issuer"):
                compile_model(model)
        raw["power_command_issuer"] = "compute-l1"
        del raw["main_power"]
        del raw["initial_main_power"]
        with self.assertRaisesRegex(CompileError, "power command issuer"):
            compile_model(model)

    def test_power_command_issuer_cannot_control_two_assemblies(self):
        from sgi_model.model_compile import _compile_io_assemblies
        model = _resolved("origin300-pci-expansion")
        compiled = compile_model(model)
        peers = copy.deepcopy(compiled["io_peers"])
        modules = copy.deepcopy(compiled["modules"])
        controllers = copy.deepcopy(compiled["controllers"])
        module_index, controller_index = len(modules), len(controllers)
        peer = dict(peers[0], id="second-peer", module_index=module_index)
        peers.append(peer)
        modules.append(dict(modules[1], id="second-module",
                            controller_index=controller_index, io_peers=[1]))
        controllers.append(dict(controllers[1], id="second-l1",
                                module_index=module_index))
        raw = model["machine"]["emulation"]["io_assemblies"][0]
        model["machine"]["emulation"]["io_assemblies"].append(dict(
            raw, peer="second-peer", module="second-module",
            controller="second-l1", placement=2))
        with self.assertRaisesRegex(CompileError, "power command issuer"):
            _compile_io_assemblies(model, {}, peers, modules, controllers)

    def test_expansion_controller_startup_is_explicit_and_typed(self):
        model = self._expansion_descriptor_model()
        self.assertNotIn("controller_startup",
                         compile_model(model)["io_assemblies"][0])
        raw = model["machine"]["emulation"]["io_assemblies"][0]
        raw["controller_startup"] = "bic-functional-experiment"
        descriptor = compile_model(model)["io_assemblies"][0]
        self.assertEqual(descriptor["controller_startup"],
                         "bic-functional-experiment")
        emitted = json.loads(render_catalogue_json([model]))["topologies"][0]
        self.assertEqual(emitted["io-assemblies"][0]["controller-startup"],
                         "bic-functional-experiment")
        raw["controller_startup"] = "physical-bus"
        with self.assertRaisesRegex(CompileError, "startup selection"):
            compile_model(model)

    def test_management_ownership_and_attachment_are_separate_from_ni(self):
        model = self._expansion_descriptor_model()
        controllers = compile_model(model)["controllers"]
        raw = {
            "owners": [{"controller": c["id"], "identity": i + 101}
                       for i, c in enumerate(controllers)],
            "associations": [{"from": 0, "to": 1, "reachable": False}],
            "attachments": [{"issuer": 0, "owner": 1}],
        }
        model["machine"]["emulation"]["l1_management"] = raw
        management = compile_model(model)["l1_management"]
        self.assertEqual(management["owners"][1], {"controller_index": 1, "identity": 102})
        self.assertFalse(management["associations"][0]["reachable"])
        self.assertEqual(management["attachments"], raw["attachments"])
        emitted = json.loads(render_catalogue_json([model]))["topologies"][0]["l1-management"]
        self.assertEqual(emitted["owners"][1]["controller-index"], 1)
        self.assertFalse(emitted["associations"][0]["reachable"])
        for mutation in ("duplicate_identity", "missing_owner", "self_association", "unassociated_attachment"):
            with self.subTest(mutation=mutation):
                changed = copy.deepcopy(model)
                m = changed["machine"]["emulation"]["l1_management"]
                if mutation == "duplicate_identity":
                    m["owners"][1]["identity"] = 101
                elif mutation == "missing_owner":
                    m["owners"].pop()
                elif mutation == "self_association":
                    m["associations"][0]["to"] = 0
                else:
                    m["attachments"][0] = {"issuer": 1, "owner": 0}
                with self.assertRaises(CompileError):
                    compile_model(changed)

    def test_expansion_descriptor_emits_typed_catalogue(self):
        model = self._expansion_descriptor_model()
        model["machine"]["emulation"]["io_assemblies"][0]["bic_identity"] = 0x123400000000c000
        model["machine"]["emulation"]["io_assemblies"][0]["initial_latch"] = 0
        model["machine"]["emulation"]["io_assemblies"][0]["intx_map"] = list(range(8)) * 6
        descriptor = json.loads(render_catalogue_json([model]))["topologies"][0]["io-assemblies"][0]
        self.assertEqual(descriptor["kind"], "origin300-pci-expansion")
        self.assertEqual(descriptor["initial-node-number"], 0)
        self.assertEqual(descriptor["bic-identity"], 0x123400000000c000)
        self.assertEqual(descriptor["initial-latch"], 0)
        self.assertEqual(descriptor["intx-map"], list(range(8)) * 6)

    def test_expansion_experiment_profiles_use_reviewed_intx_map(self):
        expected = [1, 5, 1, 5, 2, 6, 2, 6] * 6
        for name in (
            "origin300-pci-expansion",
        ):
            with self.subTest(profile=name):
                model = _resolved(name)
                descriptor = compile_model(model)["io_assemblies"][0]
                self.assertEqual(descriptor["intx_map"], expected)

    def test_expansion_descriptor_rejects_duplicates_and_missing_version(self):
        model = self._expansion_descriptor_model()
        raw = model["machine"]["emulation"]["io_assemblies"]
        raw.append(copy.deepcopy(raw[0]))
        with self.assertRaisesRegex(CompileError, "duplicate"):
            compile_model(model)
        raw.pop()
        del raw[0]["version"]
        with self.assertRaisesRegex(CompileError, "version"):
            compile_model(model)

    def test_expansion_descriptor_rejects_sibling_compute_resources(self):
        for kind in ("cpu", "memory", "flash"):
            with self.subTest(kind=kind):
                model = self._expansion_descriptor_model()
                peer = next(c for c in model["components"] if c["id"] == "io-bedrock")
                peer["parent"] = "bic-board"
                model["components"].extend([
                    {"id": "bic-board", "kind": "board", "parent": "io-enclosure"},
                    {"id": "sibling", "kind": kind, "parent": "io-enclosure"},
                ])
                with self.assertRaisesRegex(CompileError, "expansion module.*" + kind):
                    compile_model(model)

    def test_expansion_descriptor_rejects_invalid_construction_inputs(self):
        for field, value in (("peer", "bedrock"), ("module", "enclosure"),
                             ("controller", "absent"), ("placement", 0),
                             ("initial_node_number", 128), ("bedrock_revision", 1),
                             ("xbridge_revision", 1), ("head_ports", [9, 9, 11, 12]),
                             ("head_ports", [8, 9, 10, 12]),
                             ("controller_profile", "compute"), ("bic_identity", 0xa000),
                             ("intx_map", [8] * 48)):
            with self.subTest(field=field):
                model = self._expansion_descriptor_model()
                model["machine"]["emulation"]["io_assemblies"][0][field] = value
                with self.assertRaises(CompileError):
                    compile_model(model)

    def test_io_peer_l1_belongs_to_its_module(self):
        model = self._synthetic_io_peer_model()
        model["components"].append({
            "id": "io-l1", "name": "Synthetic expansion controller",
            "kind": "system-controller", "parent": "io-enclosure",
            "system_controller": {"kind": "l1"},
        })
        entry = compile_model(model)
        module = entry["modules"][1]
        controller = entry["controllers"][module["controller_index"]]
        self.assertEqual(module["nodes"], [])
        self.assertEqual(module["io_peers"], [0])
        self.assertEqual((controller["id"], controller["kind"], controller["module_index"]),
                         ("io-l1", "L1", 1))
        catalogue = json.loads(render_catalogue_json([model]))["topologies"][0]
        self.assertEqual(catalogue["controllers"][module["controller_index"]], {
            "id": "io-l1", "kind": "l1", "module-index": 1,
        })
        self.assertNotIn("node-number", catalogue["io-peers"][0])
        self.assertNotIn("nasid", catalogue["io-peers"][0])

    def test_io_peer_rejects_authored_compute_population_rows(self):
        model = self._synthetic_io_peer_model()
        model["composition"].setdefault("topology", {})["node_populations"] = [
            {"nodes": [0]},
        ]
        with self.assertRaisesRegex(CompileError, "node-population rows with I/O peers"):
            compile_model(model)

    def test_io_peer_rejects_sn0_generation(self):
        model = self._synthetic_io_peer_model("origin200")
        with self.assertRaisesRegex(CompileError, "I/O-only Bedrock peers require generation SN1"):
            compile_model(model)

    def test_pci_expansion_reference_stays_out_of_runtime_compilation(self):
        model = _resolved("origin300-pci-expansion-module")
        components = {item["id"]: item for item in model["components"]}
        peer = components["expansion-bedrock"]
        self.assertEqual((peer["kind"], peer["hardware_kind"], peer["parent"]),
                         ("bus-controller", "bedrock", "expansion-bic"))
        self.assertEqual(components["expansion-bic"]["parent"], "expansion-enclosure")
        self.assertNotIn("identity", peer)
        self.assertEqual(sum(item["kind"] == "node-asic" for item in components.values()), 1)
        self.assertFalse(any(item["kind"] in {"cpu", "memory", "flash"}
                             for cid, item in components.items() if cid.startswith("expansion-")))
        with self.assertRaisesRegex(CompileError, "model_role 'reference'"):
            compile_model(model)

    def test_node_free_module_keeps_the_origin300_node_index(self):
        model = _resolved("origin300")
        model["components"].append({
            "id": "graphics-brick", "name": "Graphics brick", "kind": "module",
        })

        entry = compile_model(model)

        self.assertEqual([module["id"] for module in entry["modules"]],
                         ["implicit-module", "graphics-brick"])
        self.assertEqual(entry["modules"][1]["nodes"], [])
        self.assertEqual(entry["nodes"][0]["module_index"], 0)

    def test_node_and_processor_shape(self):
        model = _resolved("origin200")
        entry = compile_model(model)
        self.assertEqual(len(entry["nodes"]), 1)
        node = entry["nodes"][0]
        self.assertEqual(node["asic_kind"], "HUB")
        self.assertEqual(
            set(node), {"id", "module_index", "slot", "hub_slot_bits",
                        "asic_kind", "identity", "cpu_slots", "bank_mib",
                        "accepted_memory_mib", "flash", "slave_role"})
        self.assertEqual(len(node["bank_mib"]), 8)
        slot = node["cpu_slots"][0]
        self.assertEqual(set(slot), {"interface", "slot", "assembly"})
        self.assertEqual(
            set(slot["assembly"]),
            {"id", "kind", "prid", "fpu_prid", "clock_hz", "cache_clock_hz",
             "cache_mib", "mode_bits"})

    def test_node_slot_hub_slot_bits_reach_the_catalogue_only_where_authored(self):
        # The eight-processor module's midplane wiring is QEMU's default,
        # so a node slot that authors no Hub slot bits emits no field at
        # all; one that does (the Onyx2 deskside's midplane) carries its
        # value to the node record and the catalogue.
        model = _resolved("origin2000-deskside")
        self.assertIsNone(compile_model(model)["nodes"][0]["hub_slot_bits"])
        default = json.loads(render_catalogue_json([model]))["topologies"][0]
        self.assertNotIn("hub-slot-bits", default["nodes"][0])
        board = next(item for item in model["components"]
                     if item["id"] == "node-board-1-node-board")
        board["physical_slot"]["hub_slot_bits"] = 3
        self.assertEqual(compile_model(model)["nodes"][0]["hub_slot_bits"], 3)
        node = json.loads(render_catalogue_json([model]))["topologies"][0]["nodes"][0]
        self.assertEqual(node["hub-slot-bits"], 3)

    def test_onyx2_deskside_and_rack_place_the_pipe_by_chassis(self):
        # docs/system/mips/sgi/ip27.rst: the deskside's midplane drives Hub slot
        # bits 3 and 2, puts the BaseIO-G at widget F and the pipe at
        # widget 8 with no Ktown on its chain; the rack's pipe answers at
        # IO3, Crossbow 0 widget F, with the Ktown record after the
        # geometry board's.
        # Each chassis's chain starts with its own captured geometry board:
        # a GE14-4 in the deskside, a GE16-4 in the rack.
        for name, bits, pipe_widget, baseio_widget, part, others in (
                ("onyx2-deskside", [3, 2], 8, 15, "030-1129-002", []),
                ("onyx2-rack", [None] * 4, 15, 8, "030-1398-001",
                 ["030-1067-001"])):
            with self.subTest(configuration=name):
                topology = json.loads(
                    render_catalogue_json([_resolved(name)]))["topologies"][0]
                self.assertEqual(
                    [node.get("hub-slot-bits") for node in topology["nodes"]], bits)
                boards = {board["preset"]: board for board in topology["io-boards"]}
                self.assertEqual(set(boards), {"io6g", "kona"})
                self.assertEqual(boards["kona"]["part"], part)
                self.assertEqual(boards["kona"]["other-identity-parts"], others)
                widgets = {board["component"].get("hardware_kind"):
                           (board["crossbar_index"], board["widget"])
                           for board in index_graph(_resolved(name))["io_boards"]}
                self.assertEqual(widgets, {"xg": (0, pipe_widget),
                                           "bridge": (0, baseio_widget)})

    def test_board_identity_override_replaces_the_shared_boards_part(self):
        # onyx2-rack reuses the Origin 2000 module layer, whose baseio
        # board authors the plain BaseIO part; its product layer's
        # board_identity_overrides entry states the BaseIO-G part instead.
        # Without the entry the shared part comes through.
        model = _resolved("onyx2-rack")
        board = compile_model(model)["io_boards"][0]
        self.assertEqual((board["id"], board["board_kind"], board["part"]),
                         ("baseio", "BASEIO_G", "030-0734-002"))
        del model["machine"]["emulation"]["board_identity_overrides"]
        board = compile_model(model)["io_boards"][0]
        self.assertEqual(board["part"], "030-1124-002")

    def test_flash_shape(self):
        # Which named views the flash controller answers, and their
        # addresses, are fixed by the node ASIC (SGIModelFlash's own QAPI
        # comment) and are not carried here.
        model = _resolved("origin200")
        node = compile_model(model)["nodes"][0]
        self.assertEqual(set(node["flash"]), {"kind", "size"})

    def test_bedrock_node_and_processor_shape(self):
        model = _resolved("origin300")
        entry = compile_model(model)
        self.assertEqual(len(entry["nodes"]), 1)
        node = entry["nodes"][0]
        self.assertEqual(node["asic_kind"], "BEDROCK")
        self.assertEqual(
            set(node), {"id", "module_index", "slot", "hub_slot_bits",
                        "asic_kind", "identity", "cpu_slots", "bank_mib",
                        "accepted_memory_mib", "flash", "slave_role",
                        "bedrock"})
        self.assertEqual(len(node["bank_mib"]), 8)
        # The Hub's own embedded system controller fields are unset for a
        # Bedrock node (its controller role belongs to the chassis L1).
        identity = node["identity"]
        self.assertFalse(identity["i2c_controller"])
        self.assertFalse(identity["system_controller"])
        assembly = node["cpu_slots"][0]["assembly"]
        self.assertEqual(assembly["kind"], "R14000")
        self.assertEqual(
            set(assembly),
            {"id", "kind", "prid", "fpu_prid", "clock_hz", "cache_clock_hz",
             "cache_mib", "mode_bits"})
        # No held source gives Origin 300's FPU identification register;
        # zero selects the CPU model's own default (the QEMU header rule).
        self.assertEqual(assembly["fpu_prid"], 0)

    def test_module_shape(self):
        model = _resolved("origin200")
        module = compile_model(model)["modules"][0]
        self.assertEqual(
            set(module),
            {"id", "serial", "initial_module_number", "controller_index", "nodes"})
        self.assertEqual(module["nodes"], [0])

    def test_io_board_shape(self):
        # A PCI function's own position, interrupt input and subsystem id
        # are device facts sgi-baseio's preset table already carries and
        # are not carried here. An IOC3 function does keep its Ethernet
        # population bit: a board can fit two IOC3s where only one is wired
        # for Ethernet (a BaseIO-G MediaIO daughtercard), and the
        # catalogue's offerings must count one network adapter, not two.
        model = _resolved("origin200")
        board = compile_model(model)["io_boards"][0]
        self.assertEqual(
            set(board),
            {"id", "module_index", "board_kind", "slot_index", "label",
             "revision", "serial", "part", "other_identities", "pci", "ioc3_serial"})
        # Onboard I/O sits in no XIO slot, so it has no slot name.
        self.assertIsNone(board["label"])
        ioc3 = next(item for item in board["pci"] if item["kind"] == "IOC3")
        self.assertEqual(set(ioc3), {"kind", "ethernet"})
        self.assertTrue(ioc3["ethernet"])
        self.assertIsNotNone(board["ioc3_serial"])

    def test_router_and_crossbar_shape_from_the_graph_index(self):
        # origin2000-rack has both, so this reads the typed graph index
        # directly, over real fixture data.
        model = _resolved("origin2000-rack")
        graph = index_graph(model)
        self.assertEqual(len(graph["routers"]), 2)
        self.assertEqual(len(graph["crossbars"]), 2)
        self.assertEqual(
            {(link["ends"][0]["kind"], link["ends"][1]["kind"])
             for link in graph["links"]},
            {("ROUTER", "NODE"), ("ROUTER", "ROUTER")},
        )
        # Five widget occupants: the BaseIO board's Bridge behind
        # Crossbow 1 (widget 8, hosted by node 1) and each of the four
        # Hubs' own I/O port entering its midplane Crossbow -- nodes 1
        # and 3 on Crossbow 1 (widgets 9 and 10), nodes 2 and 4 on
        # Crossbow 2 (widgets 10 and 9).
        self.assertEqual(
            {(a["crossbar_index"], a["widget"], a["node_index"])
             for a in graph["xio_attachments"]},
            {(0, 8, 0), (0, 9, 0), (0, 10, 2), (1, 10, 1), (1, 9, 3)},
        )
        self.assertEqual(len(graph["xio_attachments"]), 5)

    def test_supported_cpu_populations_shape(self):
        model = _resolved("origin200")
        entry = compile_model(model)
        self.assertEqual(entry["supported_cpu_populations"], [[1], [2]])

    def test_routed_module_compiles_routers_crossbars_and_a_controller(self):
        # origin2000-rack: four nodes, two routers, two crossbars, one
        # module system controller (the MSC) shared across all four nodes.
        entry = compile_model(_resolved("origin2000-rack"))
        self.assertEqual(len(entry["nodes"]), 4)
        self.assertEqual(len(entry["routers"]), 2)
        self.assertEqual(len(entry["crossbars"]), 2)
        self.assertEqual(len(entry["controllers"]), 1)
        self.assertEqual(entry["controllers"][0], {
            "id": "module-msc", "kind": "MSC", "module_index": 0,
            "msc_address": 0x08, "nvram_address": 0x50,
            "nvram_address_count": 8, "nvram_bytes": 2048})
        self.assertEqual([module["controller_index"] for module in entry["modules"]], [0])
        router = entry["routers"][0]
        self.assertEqual(set(router), {"id", "module_index", "slot", "port_count", "part", "revision", "chip_inputs", "serial"})
        self.assertEqual(router["id"], "router-1")
        self.assertEqual(router["module_index"], 0)
        self.assertEqual(router["slot"], 1)
        self.assertEqual(router["port_count"], 6)
        self.assertEqual(router["part"], 1)
        self.assertEqual(router["revision"], 1)
        self.assertEqual(router["chip_inputs"], 15)
        # Both routers' serials are the emulator's synthetic identities,
        # the same mechanism a
        # node's Hub uses -- never the identity block's own absent serial.
        self.assertEqual(router["serial"], 0x51454d555200)
        self.assertEqual(entry["routers"][1]["id"], "router-2")
        self.assertEqual(entry["routers"][1]["serial"], 0x51454d555201)
        # A crossbar's manufacturer id, part number and initially-
        # populated-links mask are fixed by which chip it is
        # and are not carried here.
        crossbar = entry["crossbars"][0]
        self.assertEqual(
            set(crossbar), {"id", "module_index", "kind", "widget_revision"})
        self.assertEqual(crossbar["id"], "crossbow-0")
        self.assertEqual(crossbar["module_index"], 0)
        self.assertEqual(crossbar["kind"], "XBOW")
        self.assertEqual(crossbar["widget_revision"], 2)
        self.assertEqual(len(entry["links"]), 5)
        self.assertEqual(
            {(link["kind"], link["ends"][0]["kind"], link["ends"][1]["kind"])
             for link in entry["links"]},
            {("midplane", "ROUTER", "NODE"), ("midplane", "ROUTER", "ROUTER")},
        )

    def test_dual_module_compiles_two_directly_linked_nodes(self):
        # The peer CrayLink is a direct cable. No router or crossbar is
        # involved, so the compiler uses the flat Origin 200 node path twice.
        entry = compile_model(_resolved("origin200-dual"))
        self.assertEqual(len(entry["nodes"]), 2)
        self.assertEqual(len(entry["io_boards"]), 2)
        self.assertEqual(entry["links"], [{
            "name": "peer-craylink", "kind": "cable",
            "ends": [{"kind": "NODE", "index": 0, "port": 0},
                     {"kind": "NODE", "index": 1, "port": 0}],
        }])
        self.assertEqual(
            [node["identity"]["serial"] for node in entry["nodes"]],
            [0x51454d554e00, 0x51454d554e01],
        )
        self.assertEqual(
            [board["serial"] for board in entry["io_boards"]],
            [0x51454d554200, 0x51454d554201],
        )
        # Both boards share their node's own system board (the Origin 200
        # family's IP29), so both compile to IP29_IO, never BASEIO.
        self.assertEqual(
            [board["board_kind"] for board in entry["io_boards"]],
            ["IP29_IO", "IP29_IO"],
        )

    def test_routed_module_baseio_carries_its_fitted_ioc3s_serial(self):
        # The BaseIO's IOC3 population (Ethernet, both serial ports, the
        # Timekeeper, the identity NIC) is sgi-baseio's own preset fact
        # now and is not carried here; the IOC3's own
        # fitted-part one-wire serial has no other home and survives.
        entry = compile_model(_resolved("origin2000-rack"))
        board = entry["io_boards"][0]
        self.assertEqual(board["id"], "baseio")
        # This is the Origin 2000 family's separate IO6-type BaseIO
        # board in physical IO1. The default origin2000
        # module fits the plain BaseIO -- no MediaIO daughtercard -- so it
        # compiles to the BASEIO board kind, differing from the Origin 200
        # family's onboard I/O, which shares its node's own board and
        # compiles to IP29_IO instead (see the hand-written-entry tests).
        self.assertEqual(board["board_kind"], "BASEIO")
        self.assertIsNotNone(board["ioc3_serial"])

    def test_routed_module_baseio_fits_one_ioc3_wired_for_ethernet(self):
        # The default origin2000 module's BaseIO board carries only its
        # own IOC3, wired for Ethernet. The compiled pci list keeps that
        # IOC3's Ethernet population bit so the catalogue offers one
        # network adapter.
        board = compile_model(_resolved("origin2000-rack"))["io_boards"][0]
        ethernet_flags = sorted(
            item["ethernet"] for item in board["pci"] if item["kind"] == "IOC3")
        self.assertEqual(ethernet_flags, [True])

    def test_baseio_g_follows_the_fitted_mediaio_and_stock_keeps_baseio(self):
        # The Onyx2 deskside declares the BaseIO-G: the MediaIO
        # daughtercard's IOC3, with its PS/2 keyboard and mouse, sits at PCI
        # device 6 behind the BaseIO's Bridge. That compiles to the io6g
        # preset. The stock Origin 2000 topologies, a shipped machine's
        # configuration, fit the plain io6 BaseIO and no graphics.
        model = _resolved("onyx2-deskside")
        mediaio = next(
            item for item in model["connections"]
            if item["component"] == "mediaio-ioc3" and item["bus"] == "baseio-pci")
        self.assertEqual((mediaio["pci_device"], mediaio.get("pci_function", 0)),
                         (6, 0))
        self.assertIn(
            ("mediaio-keyboard-mouse", "ioc3-keyboard-mouse", "mediaio-ioc3"),
            {(item["id"], item.get("hardware_kind"), item.get("parent"))
             for item in model["components"]})
        board = compile_model(model)["io_boards"][0]
        self.assertEqual((board["id"], board["board_kind"]), ("baseio", "BASEIO_G"))
        self.assertEqual(
            [(item["kind"], item.get("ethernet")) for item in board["pci"]],
            [("ISP1040", None), ("ISP1040", None), ("IOC3", True), ("IOC3", False)])

        models = [_resolved(name) for name in (
            "origin2000-rack", "origin2000-deskside")]
        catalogue = json.loads(render_catalogue_json(models))
        defaults = {o["default-topology"] for o in catalogue["offerings"]
                    if o["product"] == "origin2000"}
        self.assertEqual(defaults, {"origin2000-rack"})
        presets = {t["name"]: [b["preset"] for b in t["io-boards"]]
                   for t in catalogue["topologies"]}
        self.assertEqual(presets["origin2000-rack"], ["io6"])
        self.assertEqual(presets["origin2000-deskside"], ["io6"])


def _whole_chassis(model: dict) -> dict:
    """Drop the chassis's node-population rows, so it compiles as the one
    configuration every node board fitted. The fixtures below fit a board
    in IO2, which the rack module's smaller rows give to another node;
    what they test is the board, not the rows.
    """
    model["composition"]["topology"].pop("node_populations", None)
    return model


def _fit_occupant(model: dict, *, board_kind: str = "pci-shoehorn",
                  bridge_attachment: dict | None = None) -> dict:
    """Build an occupied-connector fixture for the model compiler."""
    sources = ["occupied-connector compiler fixture"]
    _whole_chassis(model)
    model["components"] += [
        {"id": "io2-board", "name": "Fitted board", "kind": "board",
         "hardware_kind": board_kind, "parent": "pci-expansion",
         "physical_slot": {"kind": "io", "number": 2},
         "identity": {"part": "030-1275-002"}, "provenance": "derived"},
        {"id": "io2-bridge", "name": "Bridge", "kind": "pci-host-bridge",
         "hardware_kind": "bridge", "pci_host_bridge": {"heads": 1},
         "identity": {"widget_part": "0xc002", "widget_mfg": "0x36",
                      "widget_revision": 4},
         "parent": "io2-board", "provenance": "derived"},
        {"id": "io2-ioc3", "name": "IOC3", "kind": "io-controller",
         "hardware_kind": "ioc3", "parent": "io2-board", "provenance": "derived"},
        {"id": "io2-ioc3-ethernet", "name": "Ethernet", "kind": "subfunction",
         "hardware_kind": "ioc3-ethernet", "parent": "io2-ioc3",
         "provenance": "derived"},
        {"id": "io2-ioc3-keyboard-mouse", "name": "Keyboard and mouse",
         "kind": "subfunction", "hardware_kind": "ioc3-keyboard-mouse",
         "parent": "io2-ioc3", "provenance": "derived"},
    ]
    model["buses"].append({"id": "io2-pci", "name": "Fitted board PCI",
                           "kind": "pci", "master": "io2-bridge",
                           "provenance": "derived"})
    upstream = {"component": "io2-bridge", "bus": "xbow0-io2",
                "role": "bridge-upstream", "provenance": "derived",
                "sources": sources}
    upstream.update(bridge_attachment or {})
    model["connections"] += [
        upstream,
        {"component": "io2-bridge", "bus": "io2-pci", "role": "bridge-downstream",
         "provenance": "derived", "sources": sources},
        {"component": "io2-ioc3", "bus": "io2-pci", "role": "endpoint",
         "pci_device": 0, "pci_function": 0, "provenance": "derived",
         "sources": sources},
    ]
    for selection in model["composition"]["connector_population"]:
        if selection["component"] == "pci-expansion":
            selection["occupancy"] = "occupied"
            selection["occupant"] = "io2-board"
    model["machine"]["emulation"]["synthetic_identities"] += [
        {"component": "io2-bridge", "serial": "0x51454d55420d",
         "reason": "fixture"},
        {"component": "io2-ioc3", "serial": "0x5e7454d5500e", "reason": "fixture"},
    ]
    return model


def _fit_widget_occupant(model: dict, *, hardware_kind: str = "xg",
                         board_kind: str = "kona", widget_revision: int = 0) -> dict:
    """Fit a bridge-less XIO widget board into origin2000-rack's IO2
    connector: XG, the InfiniteReality pipe's XIO-to-graphics ASIC
    (`_XIO_WIDGET_KINDS`), attached to the connector's XIO bus
    directly with role endpoint, carrying no PCI host bridge and no PCI
    bus of its own. The fixture uses the endpoint role and is built in
    memory for the same reason `_fit_occupant` is: exercising the
    compiler independently of any one composition file.
    """
    sources = ["occupied-connector compiler fixture"]
    _whole_chassis(model)
    model["components"] += [
        {"id": "io2-board", "name": "Fitted widget board", "kind": "board",
         "hardware_kind": board_kind, "parent": "pci-expansion",
         "physical_slot": {"kind": "io", "number": 2}, "provenance": "derived"},
        {"id": "io2-widget", "name": "Fitted widget", "kind": "io-controller",
         "hardware_kind": hardware_kind, "parent": "io2-board",
         "identity": {"widget_mfg": "0x2aa", "widget_part": "0xc102",
                      "widget_revision": widget_revision},
         "provenance": "derived"},
    ]
    model["connections"].append(
        {"component": "io2-widget", "bus": "xbow0-io2", "role": "endpoint",
         "provenance": "derived", "sources": sources})
    for selection in model["composition"]["connector_population"]:
        if selection["component"] == "pci-expansion":
            selection["occupancy"] = "occupied"
            selection["occupant"] = "io2-board"
    model["machine"]["emulation"]["synthetic_identities"].append(
        {"component": "io2-widget", "serial": "0x51454d55470d", "reason": "fixture"})
    return model


def _fit_widget_occupant_one_wire_chain(model: dict) -> dict:
    """Extend `_fit_widget_occupant`'s XG-style widget with a one-wire
    identity chain answering for two boards in an Onyx2 rack pipe:
    a sub-board fitted on the widget's own
    board, and a separate board beside it under the same connector (the
    rack's Ktown), both reached through the widget's own one-wire master.
    """
    model = _fit_widget_occupant(model)
    sources = ["one-wire identity compiler fixture"]
    model["components"] += [
        {"id": "io2-geometry-board", "name": "Fitted sub-board", "kind": "board",
         "hardware_kind": "geometry-board", "parent": "io2-board",
         "provenance": "derived"},
        {"id": "io2-geometry-identity", "name": "Sub-board one-wire identity",
         "kind": "identity", "parent": "io2-geometry-board",
         "identity": {"part": "030-1129-001"}, "provenance": "derived"},
        {"id": "io2-ktown", "name": "Cabled link board", "kind": "board",
         "hardware_kind": "ktown", "parent": "pci-expansion",
         "provenance": "derived"},
        {"id": "io2-ktown-identity", "name": "Link board one-wire identity",
         "kind": "identity", "parent": "io2-ktown",
         "identity": {"part": "030-1067-001"}, "provenance": "derived"},
    ]
    model["buses"].append(
        {"id": "io2-one-wire", "name": "Widget one-wire identity chain",
         "kind": "one-wire", "master": "io2-widget", "provenance": "derived",
         "sources": sources})
    model["connections"] += [
        {"component": "io2-widget", "bus": "io2-one-wire", "role": "master",
         "provenance": "derived", "sources": sources},
        {"component": "io2-geometry-identity", "bus": "io2-one-wire",
         "role": "endpoint", "provenance": "derived", "sources": sources},
        {"component": "io2-ktown-identity", "bus": "io2-one-wire",
         "role": "endpoint", "provenance": "derived", "sources": sources},
    ]
    return model


class OccupiedConnectorTests(unittest.TestCase):
    """An occupied connector compiles to the fitted board, not a slot."""

    def test_occupant_compiles_to_an_io_board_at_the_connectors_widget(self):
        entry = compile_model(_fit_occupant(_resolved("origin2000-rack")))
        self.assertEqual([board["id"] for board in entry["io_boards"]],
                         ["baseio", "io2-board"])
        board = entry["io_boards"][1]
        self.assertEqual(board["board_kind"], "PCI_SHOEHORN")
        self.assertEqual(board["part"], "030-1275-002")
        self.assertEqual(board["revision"], 4)
        self.assertEqual(board["slot_index"], 2)
        self.assertEqual(board["serial"], 0x51454d55420d)
        self.assertEqual(board["ioc3_serial"], 0x5e7454d5500e)
        self.assertEqual(board["pci"], [{"kind": "IOC3", "ethernet": True}])
        # The connector's own placement: crossbow 0, widget 12, controlled
        # by node-board-3 (node index 2).
        self.assertIn({
            "crossbar_index": 0, "widget": 12, "node_index": 2,
            "decoded_widget_mask": 0, "discovery_widget": 0},
            entry["xio_attachments"])
        self.assertNotIn("pci-expansion",
                         [slot["id"] for slot in entry["xio_slots"]])

    def test_unoccupied_connector_still_compiles_to_a_slot(self):
        entry = compile_model(_resolved("origin2000-rack"))
        slot = next(slot for slot in entry["xio_slots"]
                    if slot["id"] == "pci-expansion")
        self.assertEqual((slot["crossbar_index"], slot["widget"]), (0, 12))
        self.assertEqual([board["id"] for board in entry["io_boards"]],
                         ["baseio"])

    def test_occupant_attachment_cannot_carry_its_own_placement(self):
        for field, value in (("widget", 12), ("controlled_by", "node-board-3"),
                             ("xio", {"physical_port": 4})):
            with self.subTest(field=field):
                model = _fit_occupant(_resolved("origin2000-rack"),
                                      bridge_attachment={field: value})
                with self.assertRaisesRegex(CompileError, "from connector 'pci-expansion'"):
                    compile_model(model)

    def test_occupant_needs_one_bridge_on_the_connectors_bus(self):
        model = _fit_occupant(_resolved("origin2000-rack"))
        model["connections"] = [item for item in model["connections"]
                                if item["component"] != "io2-bridge"
                                or item["bus"] != "xbow0-io2"]
        model["connections"].append({"component": "io2-board", "bus": "xbow0-io2",
                                     "role": "endpoint"})
        with self.assertRaisesRegex(CompileError, "no Bridge on"):
            compile_model(model)

    def test_occupied_connector_needs_a_widget_number(self):
        model = _fit_occupant(_resolved("origin2000-rack"))
        for item in model["connections"]:
            if item["component"] == "pci-expansion":
                del item["widget"]
        with self.assertRaisesRegex(CompileError,
                                    "occupied connector 'pci-expansion' has no widget number"):
            compile_model(model)

    def test_occupant_with_two_bridges_on_the_connectors_bus_is_refused(self):
        model = _fit_occupant(_resolved("origin2000-rack"))
        model["components"].append(
            {"id": "io2-bridge-b", "name": "Second Bridge", "kind": "pci-host-bridge",
             "hardware_kind": "bridge", "parent": "io2-board", "provenance": "derived"})
        model["connections"].append(
            {"component": "io2-bridge-b", "bus": "xbow0-io2", "role": "bridge-upstream"})
        with self.assertRaisesRegex(CompileError, "more than one Bridge"):
            compile_model(model)

    def test_board_order_must_follow_attachment_order(self):
        # QEMU pairs board attachments with io_boards by position. A Bridge
        # id sorting before the BaseIO's would put the fitted board first
        # in io_boards but second in attachment order.
        model = _fit_occupant(_resolved("origin2000-rack"))
        for item in model["components"]:
            if item["id"] == "io2-bridge":
                item["id"] = "aa-bridge"
        for item in model["connections"]:
            if item["component"] == "io2-bridge":
                item["component"] = "aa-bridge"
        for item in model["buses"]:
            if item.get("master") == "io2-bridge":
                item["master"] = "aa-bridge"
        for item in model["machine"]["emulation"]["synthetic_identities"]:
            if item["component"] == "io2-bridge":
                item["component"] = "aa-bridge"
        with self.assertRaisesRegex(CompileError, "attachment order"):
            compile_model(model)

    def test_board_kind_follows_the_board_not_its_ioc3(self):
        # The fitted IOC3 has a keyboard and mouse, which alone would read
        # as a BaseIO-G MediaIO; the board's own hardware kind wins.
        entry = compile_model(_fit_occupant(_resolved("origin2000-rack")))
        self.assertEqual(entry["io_boards"][0]["board_kind"], "BASEIO")
        self.assertEqual(entry["io_boards"][1]["board_kind"], "PCI_SHOEHORN")

    def test_pci_carrier_is_an_xio_card_with_its_own_preset(self):
        model = _fit_occupant(_resolved("origin2000-rack"), board_kind="pci-carrier")
        entry = compile_model(model)
        self.assertEqual(entry["io_boards"][1]["board_kind"], "PCI_CARRIER")
        board = json.loads(render_catalogue_json([model]))["topologies"][0]["io-boards"][1]
        self.assertEqual((board["preset"], board["carrier"]), ("pci-carrier", "xio-slot"))

    def test_menet_is_an_xio_card_with_its_own_preset(self):
        model = _fit_occupant(_resolved("origin2000-rack"), board_kind="menet")
        entry = compile_model(model)
        self.assertEqual(entry["io_boards"][1]["board_kind"], "MENET")
        board = json.loads(render_catalogue_json([model]))["topologies"][0]["io-boards"][1]
        self.assertEqual((board["preset"], board["carrier"]), ("menet", "xio-slot"))

    def test_mscsi_is_an_xio_card_with_its_own_preset(self):
        model = _fit_occupant(_resolved("origin2000-rack"), board_kind="mscsi")
        entry = compile_model(model)
        self.assertEqual(entry["io_boards"][1]["board_kind"], "MSCSI")
        board = json.loads(render_catalogue_json([model]))["topologies"][0]["io-boards"][1]
        self.assertEqual((board["preset"], board["carrier"]), ("mscsi", "xio-slot"))

    def test_unknown_board_kind_is_refused(self):
        model = _fit_occupant(_resolved("origin2000-rack"), board_kind="mystery")
        with self.assertRaisesRegex(CompileError, "board kind 'mystery'"):
            compile_model(model)

    def test_widget_occupant_with_no_bridge_compiles_to_an_io_board(self):
        # XG-style occupant: no pci-host-bridge component anywhere on
        # the board, attached to the connector's XIO bus directly.
        entry = compile_model(_fit_widget_occupant(_resolved("origin2000-rack")))
        self.assertEqual([board["id"] for board in entry["io_boards"]],
                         ["baseio", "io2-board"])
        board = entry["io_boards"][1]
        self.assertEqual(board["board_kind"], "KONA")
        self.assertEqual(board["slot_index"], 2)
        self.assertEqual(board["serial"], 0x51454d55470d)
        self.assertEqual(board["pci"], [])
        self.assertIsNone(board["ioc3_serial"])
        # Same connector placement `_fit_occupant`'s Bridge-carrying board
        # gets: the widget takes the connector's own widget/controlling
        # node, not one of its own.
        self.assertIn({
            "crossbar_index": 0, "widget": 12, "node_index": 2,
            "decoded_widget_mask": 0, "discovery_widget": 0},
            entry["xio_attachments"])
        self.assertNotIn("pci-expansion",
                         [slot["id"] for slot in entry["xio_slots"]])

    def test_kona_board_kind_has_its_own_xio_preset(self):
        model = _fit_widget_occupant(_resolved("origin2000-rack"))
        entry = compile_model(model)
        self.assertEqual(entry["io_boards"][1]["board_kind"], "KONA")
        board = json.loads(render_catalogue_json([model]))["topologies"][0]["io-boards"][1]
        self.assertEqual((board["preset"], board["carrier"]), ("kona", "xio-slot"))

    def test_mgras_is_a_catalogue_board_at_its_connectors_widget(self):
        # The SI graphics board, an HQ4 widget with no Bridge, compiles like
        # any other fitted XIO board: the mgras preset, at the widget its
        # connector names, and no longer an empty slot there.
        model = _fit_widget_occupant(_resolved("origin2000-rack"),
                                     hardware_kind="hq4", board_kind="mgras")
        topology = json.loads(render_catalogue_json([model]))["topologies"][0]
        boards = [b for b in topology["io-boards"] if b["preset"] == "mgras"]
        self.assertEqual([(b["carrier"], b["label"]) for b in boards],
                         [("xio-slot", "io2")])
        self.assertIn({"crossbar-index": 0, "widget": 12, "node-index": 2},
                      topology["xio-attachments"])
        self.assertNotIn("pci-expansion",
                         {s["id"] for s in topology["xio-slots"]})

    def test_no_mgras_where_no_connector_holds_one(self):
        for name in ("origin200-gigachannel", "onyx2-rack", "origin2000-rack",
                     "origin2000-deskside"):
            with self.subTest(configuration=name):
                topology = json.loads(
                    render_catalogue_json([_resolved(name)]))["topologies"][0]
                self.assertNotIn("mgras", [b["preset"] for b in topology["io-boards"]])

    def test_widget_occupant_one_wire_chain_resolves_both_boards_parts(self):
        # A one-wire chain can answer for more than one board (the Onyx2
        # rack pipe's geometry-board and Ktown records): the first record
        # becomes the compiled part, and the Ktown's, on a board with no
        # identity of its own, joins it as a further identity -- not
        # dropped, the way a separately identified board sharing the same
        # wire (`CrosstownTests`) is.
        entry = compile_model(
            _fit_widget_occupant_one_wire_chain(_resolved("origin2000-rack")))
        board = entry["io_boards"][1]
        self.assertEqual(board["part"], "030-1129-001")
        self.assertEqual(board["other_identities"], ["030-1067-001"])

    def test_widget_revision_only_changes_its_own_field(self):
        # kona-pipe.yaml's xg identity carries widget_revision: 0 as an
        # acknowledged placeholder (no revision-field check is documented
        # anywhere the pipe's datasheet read; classification of this board
        # kind is by widget_mfg/widget_part and the "+0x64" presence check
        # alone). This pins that placeholder as inert at the catalogue/
        # compiler boundary: changing it changes only the compiled
        # "revision" field, never board_kind, part resolution, the xio
        # preset/carrier, or the XIO attachment -- the fields an
        # actual board-type dispatch would have to touch. It does not and
        # cannot reach the QEMU-side KLCONFIG/PROM board-type dispatch
        # itself, which this catalogue-only change does not implement (see
        # kona-pipe.yaml's own xg notes).
        baseline = compile_model(_fit_widget_occupant(_resolved("origin2000-rack")))
        varied = compile_model(
            _fit_widget_occupant(_resolved("origin2000-rack"), widget_revision=7))
        base_board, var_board = baseline["io_boards"][1], varied["io_boards"][1]
        self.assertEqual(base_board["revision"], 0)
        self.assertEqual(var_board["revision"], 7)
        for field in ("board_kind", "slot_index", "part", "other_identities",
                      "serial", "pci", "ioc3_serial"):
            self.assertEqual(base_board[field], var_board[field], field)
        self.assertEqual(baseline["xio_attachments"], varied["xio_attachments"])

    def test_widget_occupant_of_an_undeclared_kind_is_refused(self):
        # An occupant with no pci-host-bridge anywhere on its board and an
        # undeclared hardware_kind (not in `_XIO_WIDGET_KINDS`) attaches to
        # the connector's bus but is not recognised as its Bridge/widget --
        # the same refusal a wholly missing attachment gets. This is the
        # widening's refusal boundary: a widget kind must be declared to
        # occupy a bridge-less connector.
        model = _fit_widget_occupant(_resolved("origin2000-rack"),
                                     hardware_kind="mystery-widget")
        with self.assertRaisesRegex(CompileError, "no Bridge on"):
            compile_model(model)

    def test_catalogue_names_the_preset_and_counts_only_base_io_timekeepers(self):
        model = _fit_occupant(_resolved("origin2000-rack"))
        catalogue = json.loads(render_catalogue_json([model]))
        board = catalogue["topologies"][0]["io-boards"][1]
        self.assertEqual(board["preset"], "pci-shoehorn")
        self.assertEqual(board["carrier"], "xio-slot")
        self.assertEqual(board["attachment"], 2)
        offering = catalogue["offerings"][0]
        # The fitted IOC3 carries Ethernet, so the launcher sees a second
        # network; it has no Timekeeper, so the NVRAM count stays at the
        # BaseIO's one.
        self.assertEqual(len(offering["networks"]), 2)
        self.assertEqual([item["name"] for item in offering["storage"]
                          if item["name"].startswith("nvram")],
                         ["nvram0", "nvram0-clock"])


def _fit_crosstown(model: dict) -> dict:
    """Fit the GIGAchannel's Crosstown board on origin200's IP29 board.

    The board replaces the XTalk jumper and adds its own record on the
    IP29 Bridge's one-wire line. It is a passive adapter, so an XIO
    bus from the Hub to it, and one it masters itself, carry no widget.
    """
    sources = ["crosstown compiler fixture"]
    model["components"] += [
        {"id": "crosstown", "name": "Crosstown adapter", "kind": "board",
         "hardware_kind": "crosstown", "parent": "ip29-system-board",
         "identity": {"part": "030-1264-001"}, "provenance": "derived"},
        {"id": "crosstown-identity", "name": "Crosstown record",
         "kind": "identity", "parent": "crosstown",
         "identity": {"part": "030-1264-001"}, "provenance": "derived"},
        {"id": "crosstown-far", "name": "Far adapter", "kind": "board",
         "parent": "ip29-system-board", "provenance": "derived"},
    ]
    model["buses"] += [
        {"id": "hub-to-crosstown", "name": "Hub to Crosstown", "kind": "xio",
         "master": "hub", "provenance": "derived", "sources": sources},
        {"id": "crosstown-cable", "name": "XIO cable", "kind": "xio",
         "master": "crosstown", "provenance": "derived", "sources": sources},
    ]
    # The Crosstown record precedes the IP29's on the line, so the IP29
    # part must be chosen for the Bridge by more than connection order.
    model["connections"][:0] = [
        {"component": "crosstown-identity", "bus": "bridge-one-wire",
         "role": "endpoint", "provenance": "derived", "sources": sources},
    ]
    model["connections"] += [
        {"component": "hub", "bus": "hub-to-crosstown", "role": "master",
         "provenance": "derived", "sources": sources},
        {"component": "crosstown", "bus": "hub-to-crosstown", "role": "endpoint",
         "provenance": "derived", "sources": sources},
        {"component": "crosstown", "bus": "crosstown-cable", "role": "master",
         "provenance": "derived", "sources": sources},
        {"component": "crosstown-far", "bus": "crosstown-cable",
         "role": "endpoint", "provenance": "derived", "sources": sources},
    ]
    return model


class CrosstownTests(unittest.TestCase):
    """The GIGAchannel's Crosstown board on an Origin 200's IP29 board."""

    def test_fitted_crosstown_selects_its_own_ip29_preset(self):
        plain = compile_model(_resolved("origin200"))
        entry = compile_model(_fit_crosstown(_resolved("origin200")))
        self.assertEqual(plain["io_boards"][0]["board_kind"], "IP29_IO")
        board = entry["io_boards"][0]
        self.assertEqual(board["board_kind"], "IP29_IO_CROSSTOWN")
        # The board record the Bridge answers for stays the IP29's own.
        self.assertEqual(board["part"], plain["io_boards"][0]["part"])

    def test_passive_adapter_links_carry_no_widget(self):
        plain = compile_model(_resolved("origin200"))
        entry = compile_model(_fit_crosstown(_resolved("origin200")))
        self.assertEqual(entry["xio_attachments"], plain["xio_attachments"])

    def test_catalogue_places_it_onboard_with_its_timekeeper(self):
        model = _fit_crosstown(_resolved("origin200"))
        catalogue = json.loads(render_catalogue_json([model]))
        board = catalogue["topologies"][0]["io-boards"][0]
        self.assertEqual((board["preset"], board["carrier"]),
                         ("ip29-io-crosstown", "onboard"))
        self.assertEqual([item["name"]
                          for item in catalogue["offerings"][0]["storage"]
                          if item["name"].startswith("nvram")],
                         ["nvram0", "nvram0-clock"])

    def test_gigachannel_cabinet_is_a_machine_placed_board(self):
        model = _fit_occupant(_resolved("origin2000-rack"),
                              board_kind="gigachannel-midplane")
        board = json.loads(render_catalogue_json([model]))["topologies"][0]["io-boards"][1]
        self.assertEqual((board["preset"], board["carrier"]),
                         ("gigachannel", "machine-placed"))


class OriginTwoHundredMatchesTheHandWrittenEntryTests(unittest.TestCase):
    """Origin 200 compiled fields with fixed expected values.

    The checks cover hardware and emulator identity fields in the
    compiled result. Node and module C identifiers are not model inputs.
    """

    def setUp(self):
        self.entry = compile_model(_resolved("origin200"))

    def test_identity_and_topology_fields(self):
        self.assertEqual(self.entry["name"], "origin200")
        self.assertEqual(self.entry["description"], "SGI Origin 200 (IP29)")
        self.assertEqual(self.entry["console"], 0)
        self.assertEqual(self.entry["junk_uart"], 0)

    def test_node_identity(self):
        # The I2C controller's own bus offset and the ELSC/NVRAM addresses
        # behind it are chip constants (hw/misc/sgi-hub.c and sn0.c already
        # carry them) and are not carried here.
        identity = self.entry["nodes"][0]["identity"]
        self.assertEqual(identity, {
            "ni_revision": 3, "widget_revision": 1, "serial": 0x51454d554e00,
            "i2c_controller": True, "system_controller": True,
        })

    def test_processor(self):
        assembly = self.entry["nodes"][0]["cpu_slots"][0]["assembly"]
        self.assertEqual(assembly, {
            "id": "r10000-180mhz-1mb", "kind": "R10000",
            "prid": 0x926, "fpu_prid": 0x900,
            "clock_hz": 0xaba9500, "cache_clock_hz": 0x7270e00,
            "cache_mib": 1, "mode_bits": 0x1211a785,
        })
        self.assertIs(
            self.entry["nodes"][0]["cpu_slots"][1]["assembly"], assembly,
            "both interface slots wire the same catalogued part",
        )

    def test_bank_and_accepted_memory(self):
        node = self.entry["nodes"][0]
        self.assertEqual(node["bank_mib"], [64, 0, 0, 0, 0, 0, 0, 0])
        self.assertEqual(node["accepted_memory_mib"], [
            64, 128, 192, 256, 320, 384, 448, 512, 576, 640, 704, 768, 832,
            896, 1024, 1088, 1152, 1216, 1280, 1536, 1600, 1664, 2048])

    def test_flash(self):
        self.assertEqual(
            self.entry["nodes"][0]["flash"], {"kind": "AM29F080", "size": 0x100000})

    def test_xio_attachment(self):
        self.assertEqual(self.entry["xio_attachments"], [{
            "crossbar_index": -1, "widget": 8, "node_index": 0,
            "decoded_widget_mask": 0xfffd, "discovery_widget": 0,
        }])

    def test_module(self):
        module = self.entry["modules"][0]
        self.assertEqual(module["serial"], "")
        self.assertEqual(module["initial_module_number"], 0)
        self.assertEqual(module["controller_index"], -1)
        self.assertEqual(module["nodes"], [0])

    def test_io_board(self):
        board = self.entry["io_boards"][0]
        self.assertEqual(board["id"], "baseio")
        # This is IP29's own onboard I/O, sharing the node's system board
        # (see logic-carrier.yaml's ip29-system-board), not the Origin
        # 2000 family's own standalone BaseIO/IO6 board -- SGI_MODEL_IO_
        # BASEIO in sn-model.h names that different board.
        self.assertEqual(board["board_kind"], "IP29_IO")
        self.assertEqual(board["revision"], 4)
        self.assertEqual(board["serial"], 0x51454d554200)
        self.assertEqual(board["part"], "030-1244-001")
        # A PCI function's own device number, revision and interrupt
        # input are sgi-baseio's own preset facts now;
        # only the kind order survives, for the catalogue's offerings.
        self.assertEqual(
            [item["kind"] for item in board["pci"]], ["ISP1040", "ISP1040", "IOC3"])
        self.assertEqual(board["ioc3_serial"], 0x5e7454d55000)

    def test_supported_cpu_populations(self):
        self.assertEqual(self.entry["supported_cpu_populations"], [[1], [2]])


class OriginThreeHundredMatchesTodaysGeneratedInputTests(unittest.TestCase):
    """Field values today's generated `hw/mips/sn1-origin300-model.c`
    carries on the QEMU trunk, transcribed once as
    `.cache/sn-packets/trunk-sn1-origin300-model.c` for this comparison.

    Format 2 drops some fields on purpose rather than by omission: flash
    view addresses, the node's own XIO host port, PCI subsystem
    vendor id and the SuperIO interrupt pin all move to per-asic-kind
    node-device or device-property constants (sn-model.h's own top
    comment), so they have no format-2 counterpart to compare against.
    ni_revision and the flash kind are sourced approximations, not values
    today's generated input carries at all (it has neither field). The list
    was later widened: a PCI
    function's own device/function slot, revision, interrupt input and
    IOC3 population, and a crossbar's manufacturer id, part number and
    startup-health mask, are device facts now, and the XBridge's own
    crossbar section carries no record at all.
    """

    def setUp(self):
        self.entry = compile_model(_resolved("origin300"))

    def test_identity_and_topology_fields(self):
        self.assertEqual(self.entry["name"], "origin300")
        self.assertEqual(self.entry["description"], "SGI Origin 300 (IP45)")
        self.assertEqual(self.entry["console"], 0)
        self.assertEqual(self.entry["junk_uart"], -1)

    def test_processor(self):
        # The catalogue accepts two or four processors and wires four
        # processor-interface slots, all with a fitted assembly. -smp
        # selects the active population from this one topology.
        slots = self.entry["nodes"][0]["cpu_slots"]
        assembly = slots[0]["assembly"]
        self.assertEqual(assembly["kind"], "R14000")
        self.assertEqual(assembly["clock_hz"], 0x1dcd6500)
        self.assertEqual(assembly["mode_bits"], 0x009aa985)
        self.assertEqual(assembly["cache_mib"], 2)
        self.assertEqual(
            [(slot["interface"], slot["slot"]) for slot in slots],
            [(0, 0), (0, 1), (1, 0), (1, 1)],
        )
        self.assertEqual(
            [slot["assembly"] is assembly for slot in slots],
            [True, True, True, True],
        )

    def test_bank_and_accepted_memory(self):
        node = self.entry["nodes"][0]
        self.assertEqual(node["bank_mib"], [0, 0, 256, 256, 0, 0, 0, 0])
        self.assertEqual(node["accepted_memory_mib"], [512])

    def test_flash(self):
        # Which named views the flash controller answers are fixed by the
        # node ASIC and are not carried here (sn1-node.c names them itself).
        self.assertEqual(
            self.entry["nodes"][0]["flash"], {"kind": "AM29F080", "size": 0x1686a8})

    def test_no_crossbar_record_for_the_xbridge(self):
        # The XBridge's own crossbar section is part of the IP45 node
        # board, as the L1 and the boot flash are: the node device creates it
        # directly, so it never
        # gets a catalogue record of its own.
        self.assertEqual(self.entry["crossbars"], [])

    def test_xio_attachments(self):
        # Both PCI bridge heads attach directly to the node (no crossbar
        # record exists to index), matching the direct-attachment shape
        # Origin 200's own Bridge uses.
        self.assertEqual(
            {(item["widget"], item["node_index"], item["crossbar_index"])
             for item in self.entry["xio_attachments"]},
            {(14, 0, -1), (15, 0, -1)},
        )

    def test_io_boards(self):
        boards = {board["id"]: board for board in self.entry["io_boards"]}
        self.assertEqual(set(boards), {"xbridge-bridge-e", "xbridge-bridge-f"})
        for board in boards.values():
            self.assertEqual(board["board_kind"], "IO8")
            self.assertEqual(board["revision"], 2)
            self.assertEqual(board["part"], "030-1673-003")
        self.assertEqual(boards["xbridge-bridge-e"]["pci"], [])
        self.assertIsNone(boards["xbridge-bridge-e"]["ioc3_serial"])
        # A PCI function's own device number, revision, interrupt input
        # and IOC3 population flags are sgi-baseio's own preset facts now;
        # only the kind order survives.
        head_f = boards["xbridge-bridge-f"]
        self.assertEqual(
            [item["kind"] for item in head_f["pci"]], ["ISP12160", "IOC3", "LUCENT_OHCI"])
        self.assertEqual(head_f["ioc3_serial"], 0x5e7454d55000)

    def test_controller(self):
        self.assertEqual(self.entry["controllers"], [{
            "id": "l1", "kind": "L1", "module_index": 0,
            "l1": {"reported_type": 52,
                   "revision": {"major": 1, "minor": 18, "patch": 5}},
            "msc_address": 0, "nvram_address": 0,
            "nvram_address_count": 0, "nvram_bytes": 0,
        }])
        self.assertEqual(self.entry["modules"][0]["controller_index"], 0)

    def test_supported_cpu_populations(self):
        self.assertEqual(self.entry["supported_cpu_populations"], [[2], [4]])


class OriginThreeHundredTwoChassisTests(unittest.TestCase):
    """Two Origin 300 chassis, each with a complete base module,
    joined by one NUMAlink 3 cable. Each chassis's Bedrock gets its own
    processors, IO-8 and L1, and nothing in the compiled topology fixes a
    NASID -- the stock PROM assigns those by discovery.
    """

    def setUp(self):
        self.entry = compile_model(_resolved("origin300-two-chassis"))

    def test_one_bedrock_node_per_chassis_module(self):
        nodes = self.entry["nodes"]
        self.assertEqual([node["id"] for node in nodes],
                         ["master-bedrock", "slave-bedrock"])
        self.assertEqual([node["asic_kind"] for node in nodes], ["BEDROCK", "BEDROCK"])
        self.assertEqual([node["module_index"] for node in nodes], [0, 1])
        # Each chassis's node sits in its own module's slot 0, as both
        # captured pairs' "Module 001c0N/Slot 0" inventories place it.
        self.assertEqual([node["slot"] for node in nodes], [0, 0])
        self.assertEqual([node["slave_role"] for node in nodes], [False, True])
        self.assertEqual([node["identity"]["serial"] for node in nodes],
                         [0x51454d554e00, 0x51454d554e01])

    def test_each_node_carries_only_its_own_chassis_processors(self):
        # Four fitted slices per node, not all eight processors in the
        # model: a Bedrock's CPUs are the ones on the SysAD buses it
        # terminates.
        for node in self.entry["nodes"]:
            self.assertEqual(
                [(slot["interface"], slot["slot"]) for slot in node["cpu_slots"]
                 if slot["assembly"] is not None],
                [(0, 0), (0, 1), (1, 0), (1, 1)])
        self.assertEqual(self.entry["supported_cpu_populations"],
                         [[2, 2], [4, 2], [2, 4], [4, 4]])

    def test_each_chassis_brings_its_own_io8(self):
        boards = self.entry["io_boards"]
        self.assertEqual([board["id"] for board in boards],
                         ["master-xbridge-bridge-e", "master-xbridge-bridge-f",
                          "slave-xbridge-bridge-e", "slave-xbridge-bridge-f"])
        self.assertEqual([board["module_index"] for board in boards], [0, 0, 1, 1])
        self.assertEqual([board["serial"] for board in boards],
                         [0x51454d555800, 0x51454d555800, 0x51454d555801, 0x51454d555801])
        self.assertEqual([board["ioc3_serial"] for board in boards],
                         [None, 0x5e7454d55000, None, 0x5e7454d55001])
        self.assertEqual(
            [(item["node_index"], item["widget"]) for item in self.entry["xio_attachments"]],
            [(0, 14), (0, 15), (1, 14), (1, 15)])
        # QEMU's builder indexes attachments across the whole topology, so
        # the slave's heads follow the master's rather than restarting at 0.
        self.assertEqual([board["slot_index"] for board in boards], [0, 1, 2, 3])

    def test_one_cable_joins_the_two_bedrocks(self):
        self.assertEqual(self.entry["links"], [{
            "name": "numalink-cable", "kind": "cable",
            "ends": [{"kind": "NODE", "index": 0, "port": 0},
                     {"kind": "NODE", "index": 1, "port": 0}],
        }])
        self.assertEqual(self.entry["routers"], [])

    def test_each_module_has_its_own_l1(self):
        self.assertEqual(
            [(item["id"], item["kind"], item["module_index"])
             for item in self.entry["controllers"]],
            [("master-l1", "L1", 0), ("slave-l1", "L1", 1)])
        # Each chassis's L1 starts at its brick slot: 01 on the master, the
        # factory setting, and 02 on the slave, set before cabling.
        self.assertEqual(
            [(module["id"], module["initial_module_number"], module["controller_index"],
              module["nodes"]) for module in self.entry["modules"]],
            [("master-module", 1, 0, [0]), ("slave-module", 2, 1, [1])])

    def test_no_nasid_is_fixed(self):
        catalogue = render_catalogue_json([_resolved("origin300-two-chassis")])
        self.assertNotIn("nasid", catalogue.lower())

    def test_offerings_count_both_chassis_identity_inputs(self):
        catalogue = json.loads(render_catalogue_json([_resolved("origin300-two-chassis")]))
        offerings = catalogue["offerings"]
        self.assertEqual([offering["smp"] for offering in offerings], [4, 6, 6, 8])
        self.assertEqual({offering["default-topology"] for offering in offerings},
                         {"origin300"})
        counts = {item["argument"]: item["count"] for item in offerings[0]["resources"]
                  if item["kind"] == "identity-eeprom"}
        self.assertEqual(counts, {"chassis-eeprom.N=": 4, "board-eeprom.N=": 4})
        flash = [item for item in offerings[0]["storage"]
                 if item["name"].startswith("node")]
        self.assertEqual([(item["name"], item["size"]) for item in flash],
                         [("node0-flash", 16 * 1024 * 1024),
                          ("node1-flash", 16 * 1024 * 1024)])
        self.assertEqual(
            [(item["kind"], item["node"], item["argument"]) for item in offerings[0]["consoles"]],
            [("console", 0, "-serial chardev:l1"),
             ("system-controller", 1, "-serial chardev:l1_node1")])


class RefusalTests(unittest.TestCase):
    """Reference-role and not-yet-grounded configurations are refused."""

    def test_reference_configuration_is_refused(self):
        # The generic Origin 2000 reference model is not an emulation
        # target, so the compiler must refuse it.
        with self.assertRaisesRegex(CompileError, "not an emulation target"):
            compile_model(_resolved("origin2000"))

    def test_unsupported_generation_is_refused_by_name(self):
        model = _resolved("origin300")
        model["machine"]["generation"] = "SN2"
        with self.assertRaisesRegex(CompileError, "generation 'SN2'"):
            compile_model(model)

    def test_dual_module_compiles_with_router_assembly(self):
        # The Origin 200 pair uses a direct CrayLink cable and compiles
        # into the catalogue without a router or crossbar.
        entry = compile_model(_resolved("origin200-dual"))
        self.assertIn("modules", entry)

    def test_gigachannel_compiles_its_cabinet_crossbow(self):
        # origin200-gigachannel was the crossbar-backed reference
        # configuration this class refused by role; as an emulation
        # target it compiles its cabinet Crossbow, with the Hub on link
        # A, the IP29 Bridge on link 9 and the cabinet Bridge on link 8.
        entry = compile_model(_resolved("origin200-gigachannel"))
        self.assertEqual(len(entry["crossbars"]), 1)
        self.assertEqual(
            [(item["crossbar_index"], item["widget"], item["node_index"])
             for item in entry["xio_attachments"]],
            [(0, 10, 0), (0, 9, 0), (0, 8, 0)])
        self.assertEqual([board["board_kind"] for board in entry["io_boards"]],
                         ["IP29_IO_CROSSTOWN", "GIGACHANNEL"])
        self.assertEqual([board["part"] for board in entry["io_boards"]],
                         ["030-1244-001", "030-1252-001"])


class ReferenceModelTests(unittest.TestCase):
    def test_origin350_reference_keeps_unknown_ioc4_clock_absent(self):
        model = load_checked_system_model(
            MACHINES / "origin350-reference.yaml", model_schema=schema(MACHINES))
        ioc4 = next(component for component in model["components"]
                    if component.get("hardware_kind") == "ioc4")
        self.assertNotIn("ioc4", ioc4)

    def test_ioc4_clock_block_is_typed_and_ioc4_only(self):
        from jsonschema import Draft202012Validator

        model = _resolved("origin350")
        ioc4 = next(component for component in model["components"]
                    if component.get("hardware_kind") == "ioc4")
        validator = Draft202012Validator(schema(MACHINES))
        self.assertEqual(list(validator.iter_errors(model)), [])

        # The schema checks shape and positivity. IO9 compilation selects
        # the supported QOM clock values separately.
        ioc4["ioc4"] = {"clock_hz": 1}
        self.assertEqual(list(validator.iter_errors(model)), [])
        ioc4["ioc4"]["clock_hz"] = 0
        self.assertNotEqual(list(validator.iter_errors(model)), [])

        model = _resolved("origin300")
        validator = Draft202012Validator(schema(MACHINES))
        model["components"][0]["ioc4"] = {"clock_hz": 1}
        self.assertNotEqual(list(validator.iter_errors(model)), [])

class ManifestTests(unittest.TestCase):
    def test_manifest_format(self):
        manifest_text = render_manifest(
            {"origin200": {"a": 1}}, {"sgi-machines.json": "{}"})
        manifest = json.loads(manifest_text)
        self.assertEqual(manifest["format"], 4)
        self.assertEqual(manifest["format_key"], "sgi-sn")
        self.assertIn("source_sha256", manifest)
        self.assertIn("sgi-machines.json", manifest["outputs"])


class CatalogueJsonTests(unittest.TestCase):
    """`render_catalogue_json`'s `topologies` and `offerings` sections,
    checked against independently expected values rather than only a
    captured golden -- the packet's own worked examples.
    """

    def test_ip53_v12_slot_is_optional_with_no_default(self):
        assemblies = {"onyx350-compat": "030-1909-008",
                      "tezro-tower-multi": "030-1884-002"}
        catalogue = json.loads(render_catalogue_json(
            [_resolved(name) for name in assemblies]))
        topologies = {item["name"]: item for item in catalogue["topologies"]}

        for name, assembly in assemblies.items():
            graphics = topologies[name]["graphics"]
            self.assertTrue(graphics["headless"], name)
            self.assertEqual([slot["name"] for slot in graphics["slots"]],
                             ["widget-12"], name)
            slot = graphics["slots"][0]
            self.assertNotIn("default", slot, name)
            self.assertEqual(slot["placement"],
                             [{"name": "slot", "value": "widget-12"}], name)
            [family] = slot["families"]
            self.assertEqual(family["device"], "sgi-odyssey", name)
            [product] = family["products"]
            self.assertEqual((product["product"], product["assembly"]),
                             ("v12", assembly), name)

    def test_only_six_cleared_ip53_offerings_drop_cpu_requirements(self):
        profile_names = (
            "origin350", "onyx350-compat", "onyx4-host-serial-compat",
            "tezro-tower-one", "tezro-tower-multi", "tezro-rack",
        )
        catalogue = json.loads(render_catalogue_json(
            [_resolved(name) for name in profile_names]))
        offerings = [item for item in catalogue["offerings"]
                     if item.get("launch-recipe") == "ip53-discovery"]
        required_properties = [
            "r16000-experimental", "r16000-reset-mode",
            "r16000-scache-bytes",
        ]
        normal = {
            ("origin350", "origin350", (4,)),
            ("onyx350", "onyx350-compat", (4,)),
            ("onyx4-host", "onyx4-host-serial-compat", (4,)),
            ("tezro", "tezro-tower-one", (1,)),
            ("tezro", "tezro-tower-multi", (4,)),
            ("tezro", "tezro-rack", (1,)),
        }
        topologies = {item["name"]: item for item in catalogue["topologies"]}
        actual_normal = set()
        for offering in offerings:
            key = (offering["product"], offering["topology"],
                   tuple(offering["cpus-per-node"]))
            if "required-properties" not in offering["processor"]:
                actual_normal.add(key)
                assembly = topologies[key[1]]["nodes"][0]["cpu-slots"][0][
                    "assembly"]
                self.assertEqual(
                    assembly["required-properties"], required_properties, key,
                )
            else:
                self.assertEqual(
                    offering["processor"]["required-properties"],
                    required_properties, key,
                )
        self.assertEqual(actual_normal, normal)
        self.assertEqual(len(offerings), 9)

        tower = topologies["tezro-tower-multi"]
        self.assertEqual(
            [item["cpus-per-node"] for item in
             tower["node-populations"][0]["supported-cpu-populations"]],
            [[4], [2]],
        )
        self.assertEqual(
            _resolved("tezro-tower-multi")["composition"]["population"][
                "cpus_per_node"], [4],
        )

    def test_empty_pci_function_lists_keep_authored_presence(self):
        legacy = _emit_io_board({
            "id": "fixture-baseio", "module_index": 0,
            "board_kind": "BASEIO", "other_identities": [],
            "part": None, "revision": None, "serial": None,
            "ioc3_serial": None,
            "pci_functions": [],
        })
        self.assertEqual(legacy["pci-functions"], [])

        io9 = _emit_io_board({
            "id": "fixture-io9", "module_index": 0,
            "board_kind": "IO9", "other_identities": [], "pci": [],
            "part": None, "revision": None, "serial": None,
            "ioc3_serial": None,
        })
        self.assertEqual(io9["pci-functions"], [])

        io9_without_list = _emit_io_board({
            "id": "fixture-io9-unlisted", "module_index": 0,
            "board_kind": "IO9", "other_identities": [],
            "part": None, "revision": None, "serial": None,
            "ioc3_serial": None,
        })
        self.assertNotIn("pci-functions", io9_without_list)

    def test_document_shape(self):
        catalogue = json.loads(render_catalogue_json([_resolved("origin200")]))
        self.assertEqual(catalogue["schema"], "sgi-machines")
        self.assertEqual(catalogue["version"], 1)
        self.assertEqual(set(catalogue), {
            "schema", "version", "topologies", "offerings", "references"})
        self.assertEqual(catalogue["references"], [])

    def test_enumerations_are_lower_case_and_hyphenated(self):
        catalogue = json.loads(render_catalogue_json([_resolved("origin300")]))
        topology = catalogue["topologies"][0]
        self.assertEqual(topology["generation"], "sn1")
        self.assertEqual(topology["product"], "origin300")
        node = topology["nodes"][0]
        self.assertEqual(node["asic-kind"], "bedrock")
        self.assertEqual(node["flash"]["kind"], "am29f080")
        board = next(item for item in topology["io-boards"]
                     if item["id"] == "xbridge-bridge-f")
        self.assertEqual(board["preset"], "io8")
        self.assertEqual(board["carrier"], "machine-placed")
        self.assertIn("ioc3-serial", board)
        empty_head = next(item for item in topology["io-boards"]
                          if item["id"] == "xbridge-bridge-e")
        self.assertNotIn("ioc3-serial", empty_head)
        # No crossbar record for the XBridge: it is part of the IP45 node
        # board, not a standalone XIO switch.
        self.assertEqual(topology["crossbars"], [])
        self.assertEqual(topology["controllers"][0]["kind"], "l1")

    def test_ownership_rule_drops_device_facts(self):
        # A topology carries fitted-part identity and
        # placement, never a fact a device model already fixes for its
        # chip. Each assertion here names one field the ownership rule
        # excludes and the packet report's own list of exclusions cites.
        catalogue = json.loads(render_catalogue_json([_resolved("origin2000-rack")]))
        topology = catalogue["topologies"][0]
        node = topology["nodes"][0]
        self.assertEqual(
            set(node["identity"]),
            {"ni-revision", "widget-revision", "serial", "i2c-controller",
             "system-controller"})
        module = topology["modules"][0]
        self.assertEqual(module["identity-serial"], 0x51454D555900)
        self.assertEqual(module["midplane-serial"], 0x51454D554C00)
        self.assertEqual(module["midplane-record"],
                         {"part": "030-0762-006", "revision": "H",
                          "name": "8P12_MPLN"})
        self.assertNotIn("management-adapter-offset", node)
        self.assertEqual(set(node["flash"]), {"kind", "size"})
        board = topology["io-boards"][0]
        self.assertEqual(
            set(board) - {"ioc3-serial"},
            {"id", "module-index", "preset", "carrier", "attachment", "label",
             "part", "other-identity-parts", "revision", "serial", "populated"})
        self.assertEqual(board["label"], "io1")
        self.assertNotIn("pci", board)
        router = topology["routers"][0]
        self.assertNotIn("port-count", router)
        crossbar = topology["crossbars"][0]
        self.assertEqual(
            set(crossbar),
            {"id", "module-index", "kind", "widget-revision"})
        controller = topology["controllers"][0]
        self.assertEqual(set(controller), {"id", "module-index", "kind"})
        attachment = topology["xio-attachments"][0]
        self.assertEqual(
            set(attachment), {"crossbar-index", "widget", "node-index"})
        slots = {slot["id"]: slot for slot in topology["xio-slots"]}
        self.assertEqual(set(slots["xio-slot-5"]),
                         {"id", "crossbar-index", "widget",
                          "controlling-node", "label"})
        self.assertEqual(slots["xio-slot-5"], {
            "id": "xio-slot-5",
            "crossbar-index": 0,
            "widget": 0xe,
            "controlling-node": 0,
            "label": "io5",
        })
        # IO2 is the PCI expansion connector's own position.
        self.assertEqual(slots["pci-expansion"]["label"], "io2")
        self.assertEqual(len(topology["xio-attachments"]), 5)
        self.assertEqual(len(topology["xio-slots"]), 11)
        # SGIModelCpuPopulation wraps a population in a one-member record
        # (QAPI has no bare list-of-lists member type); a bare list here
        # is exactly the shape a JSON object visitor refuses.
        row = topology["node-populations"][-1]
        population = row["supported-cpu-populations"][0]
        self.assertEqual(set(population), {"cpus-per-node"})
        self.assertIsInstance(population["cpus-per-node"], list)

    def test_origin_200_offers_each_population_of_each_processor(self):
        # A module offered only as a dual (225 and 270 MHz) has no
        # one-processor offering; only the default spells no processor=.
        catalogue = json.loads(render_catalogue_json([_resolved("origin200")]))
        self.assertEqual(
            sorted((o["processor"]["id"], o["smp"], o["cpus-per-node"],
                    o["machine-options"], o["default-cpu-model"])
                   for o in catalogue["offerings"]),
            [("r10000-180mhz-1mb", 1, [1],
              "origin200,topology=origin200,nodes=1", "R10000"),
             ("r10000-180mhz-1mb", 2, [2],
              "origin200,topology=origin200,nodes=1", "R10000"),
             ("r10000-180mhz-2mb", 1, [1], "origin200,topology=origin200,"
              "nodes=1,processor=r10000-180mhz-2mb", "R10000"),
             ("r10000-180mhz-2mb", 2, [2], "origin200,topology=origin200,"
              "nodes=1,processor=r10000-180mhz-2mb", "R10000"),
             ("r10000-225mhz-2mb", 2, [2], "origin200,topology=origin200,"
              "nodes=1,processor=r10000-225mhz-2mb", "R10000"),
             ("r12000-270mhz-4mb", 2, [2], "origin200,topology=origin200,"
              "nodes=1,processor=r12000-270mhz-4mb", "R12000")],
        )

    def test_origin_200_memory_follows_the_processor_clock(self):
        # 256 MB banks, and with them 960 and 1792 MiB, only where the
        # PROM keeps them: not at 180 MHz, whatever the cache. Four
        # 512 MB banks make 2048 MiB at every clock.
        catalogue = json.loads(render_catalogue_json([_resolved("origin200")]))
        accepted = {o["processor"]["id"]: o["memory-per-node-mib"]["accepted"]
                    for o in catalogue["offerings"]}
        for slow in ("r10000-180mhz-1mb", "r10000-180mhz-2mb"):
            self.assertNotIn(960, accepted[slow])
            self.assertNotIn(1792, accepted[slow])
            self.assertEqual(max(accepted[slow]), 2048)
        for fast in ("r10000-225mhz-2mb", "r12000-270mhz-4mb"):
            self.assertIn(960, accepted[fast])
            self.assertIn(1792, accepted[fast])
            self.assertEqual(max(accepted[fast]), 2048)

    def test_origin_200_processor_options_carry_each_mode_word(self):
        # Expected words assembled by hand from the SysAD fields: tap,
        # cache clock divisor, cache size, system clock divisor.
        topology = compile_model(_resolved("origin200"))
        self.assertEqual(
            [(option["assembly"]["id"], option["assembly"]["kind"],
              option["assembly"]["prid"], option["assembly"]["mode_bits"],
              option["assembly"]["cache_clock_hz"])
             for option in topology["processor_options"]],
            [("r10000-180mhz-1mb", "R10000", 0x926, 0x1211a785, 120_000_000),
             ("r10000-180mhz-2mb", "R10000", 0x926, 0x100aa785, 180_000_000),
             ("r10000-225mhz-2mb", "R10000", 0x934, 0x120aa985, 225_000_000),
             ("r12000-270mhz-4mb", "R12000", 0xe23, 0x1213ab85,
              180_000_000)],
        )

    def test_origin_200_dual_smp_is_four(self):
        catalogue = json.loads(render_catalogue_json([_resolved("origin200-dual")]))
        offerings = catalogue["offerings"]
        self.assertEqual(len(offerings), 4)
        for offering in offerings:
            self.assertEqual(offering["smp"], 4)
            self.assertEqual(offering["cpus-per-node"], [2, 2])
            self.assertEqual(offering["product"], "origin200")

    def test_origin_2000_rack_default_offering_is_four_nodes_of_two(self):
        # One offering per row and population, the largest row first:
        # the machine's default is four node boards of two processors.
        catalogue = json.loads(render_catalogue_json([_resolved("origin2000-rack")]))
        offerings = catalogue["offerings"]
        self.assertEqual(offerings[0]["smp"], 8)
        self.assertEqual(offerings[0]["nodes"], 4)
        self.assertEqual(offerings[0]["product"], "origin2000")
        self.assertEqual(
            sorted({(o["nodes"], o["smp"]) for o in offerings}),
            [(1, 1), (1, 2), (2, 2), (2, 3), (2, 4), (3, 3), (3, 4), (3, 5),
             (3, 6), (4, 4), (4, 5), (4, 6), (4, 7), (4, 8)])
        for offering in offerings:
            flash = [item["name"] for item in offering["storage"]
                     if item["name"].startswith("node")]
            self.assertEqual(flash, [f"node{index}-flash"
                                     for index in range(offering["nodes"])])

    def test_every_offering_names_its_products_default_topology(self):
        # The default is the product's base composition (model_inputs'
        # MACHINE_FILENAMES), whatever other topologies the product has.
        catalogue = json.loads(render_catalogue_json([
            _resolved("origin2000-rack"), _resolved("origin200"),
            _resolved("origin200-dual")]))
        self.assertEqual(
            {(o["topology"], o["default-topology"]) for o in catalogue["offerings"]},
            {("origin2000-rack", "origin2000-rack"),
             ("origin200", "origin200"), ("origin200-dual", "origin200")})

    def test_origin_2000_module_offers_one_network(self):
        # The default origin2000 module's BaseIO board fits one IOC3,
        # wired for Ethernet, so the launcher offers one -netdev.
        catalogue = json.loads(render_catalogue_json([_resolved("origin2000-rack")]))
        networks = catalogue["offerings"][0]["networks"]
        self.assertEqual([item["kind"] for item in networks], ["ioc3-ethernet"])

    def test_origin_300_has_one_offering_per_accepted_population(self):
        # Origin 300 accepts two or four processors. The catalogue must
        # offer exactly those populations, sharing the
        # one compiled topology's maximal cpu_slots wiring, and no other.
        catalogue = json.loads(render_catalogue_json([_resolved("origin300")]))
        offerings = catalogue["offerings"]
        self.assertEqual(len(offerings), 2)
        self.assertEqual([offering["smp"] for offering in offerings], [2, 4])
        self.assertEqual(offerings[0]["cpus-per-node"], [2])
        self.assertEqual(offerings[1]["cpus-per-node"], [4])
        # Both offerings name the same catalogued 500 MHz/2 MiB part: -smp
        # selects a population out of the one compiled wiring, not a
        # different processor.
        self.assertEqual(offerings[0]["processor"], offerings[1]["processor"])

    def test_origin_300_io8_has_one_storage_controller(self):
        # The IO-8's two PCI bridge heads carry one SCSI host adapter
        # (ISP12160, head F) and no others -- head E's own pci list is
        # empty, and IOC3/LUCENT_OHCI are not storage controllers.
        catalogue = json.loads(render_catalogue_json([_resolved("origin300")]))
        adapters = catalogue["offerings"][0]["scsi-adapters"]
        self.assertEqual(len(adapters), 1)
        self.assertEqual(adapters[0]["bus"], "scsi.0")

    def test_origin_200_has_two_storage_controllers(self):
        # Both onboard ISP1040 channels are host adapters of their own.
        catalogue = json.loads(render_catalogue_json([_resolved("origin200")]))
        adapters = catalogue["offerings"][0]["scsi-adapters"]
        self.assertEqual([item["bus"] for item in adapters],
                         ["scsi.0", "scsi.1"])

    def test_consoles_follow_podlibs_line_order(self):
        # tools/podlib.py's CHARDEV_IOC3_A/IOC3_B/SYSTEM_CONTROLLER/
        # BRINGUP_PORT order.
        catalogue = json.loads(render_catalogue_json([_resolved("origin200")]))
        self.assertEqual(
            [item["kind"] for item in catalogue["offerings"][0]["consoles"]],
            ["console", "second-port", "system-controller", "junk-uart"],
        )

    def test_origin_300_console_is_a_single_l1_line(self):
        catalogue = json.loads(render_catalogue_json([_resolved("origin300")]))
        self.assertEqual(
            [item["kind"] for item in catalogue["offerings"][0]["consoles"]],
            ["console"],
        )

    def test_refused_configuration_raises_the_same_error(self):
        with self.assertRaisesRegex(CompileError, "not an emulation target"):
            render_catalogue_json([_resolved("origin2000")])


class NodePopulationTests(unittest.TestCase):
    """The node-population rows a chassis authors, compiled to positions.

    Expected values are written out from the manuals each row cites
    (108-0170-001 Figures 6-2 and 6-3, 007-3453-002 Figure 2-9), not read
    back from the YAML.
    """

    def _rows(self, name: str) -> list[dict]:
        return compile_model(_resolved(name))["node_populations"]

    def _links(self, name: str) -> list[str]:
        return [link["name"] for link in compile_model(_resolved(name))["links"]]

    def test_rack_keeps_both_routers_from_two_node_boards(self):
        entry = compile_model(_resolved("origin2000-rack"))
        links = [link["name"] for link in entry["links"]]
        rows = entry["node_populations"]
        self.assertEqual([len(row["nodes"]) for row in rows], [1, 2, 3, 4])
        self.assertEqual([row["routers"] for row in rows],
                         [[], [0, 1], [0, 1], [0, 1]])
        self.assertEqual([sorted(links[i] for i in row["cables"]) for row in rows], [
            [],
            ["r1-node1", "r1-node2", "r1-r2"],
            ["r1-node1", "r1-node2", "r1-r2", "r2-node3"],
            ["r1-node1", "r1-node2", "r1-r2", "r2-node3", "r2-node4"],
        ])
        # One node board: the manual says no Router is required, and no
        # one-node rack capture is held, so an empty Router pair is inferred.
        self.assertEqual([row["grade"] for row in rows],
                         ["INFERRED", "ESTABLISHED", "INFERRED", "ESTABLISHED"])

    def test_deskside_fits_null_router_then_two_standard_routers(self):
        entry = compile_model(_resolved("origin2000-deskside"))
        names = [link["name"] for link in entry["links"]]
        rows = entry["node_populations"]
        self.assertEqual([sorted(names[i] for i in row["cables"]) for row in rows], [
            [],
            ["null-router"],
            ["r1-node1", "r1-node2", "r1-r2", "r2-node3"],
            ["r1-node1", "r1-node2", "r1-r2", "r2-node3", "r2-node4"],
        ])
        self.assertEqual([row["routers"] for row in rows], [[], [], [0, 1], [0, 1]])
        # The deskside guide contradicts itself over a one-node deskside's
        # first Router board, and two Standard Routers are the model's
        # choice over SGI's Router and Star Router: all three are inferred.
        self.assertEqual([row["grade"] for row in rows],
                         ["INFERRED", "ESTABLISHED", "INFERRED", "INFERRED"])

    def test_one_node_row_controls_the_whole_first_bank(self):
        entry = compile_model(_resolved("origin2000-deskside"))
        labels = [slot["label"] for slot in entry["xio_slots"]]
        row = entry["node_populations"][0]
        self.assertEqual(sorted(labels[item["slot"]] for item in row["xio_slots"]),
                         ["io2", "io3", "io4", "io5", "io6"])
        self.assertEqual({item["controlling_node"] for item in row["xio_slots"]}, {0})

    def test_onyx2_deskside_rows(self):
        rows = self._rows("onyx2-deskside")
        self.assertEqual([row["nodes"] for row in rows], [[0], [0, 1]])
        self.assertEqual([row["supported_cpu_populations"][0] for row in rows],
                         [[2], [2, 2]])

    def test_a_chassis_without_rows_is_one_row(self):
        rows = self._rows("origin200-dual")
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["nodes"], [0, 1])
        self.assertEqual(rows[0]["grade"], "ESTABLISHED")

    def _refused(self, name: str, mutate, message: str) -> None:
        model = _resolved(name)
        mutate(model["composition"]["topology"]["node_populations"])
        with self.assertRaisesRegex(CompileError, message):
            compile_model(model)

    def test_a_row_attaching_an_endpoint_twice_is_refused(self):
        self._refused(
            "origin2000-deskside",
            lambda rows: rows[3]["fabric_links"].append("null-router"),
            r"row 3 attaches fabric endpoint .* twice")

    def test_a_row_leaving_a_link_end_out_is_refused(self):
        self._refused(
            "origin2000-deskside",
            lambda rows: rows[1]["fabric_links"].append("r1-node1"),
            "row 1 fits fabric link 'r1-node1' without the router")

    def test_rows_follow_fill_order(self):
        def swap(rows):
            rows[2]["node_slots"] = [1, 3, 2]
        self._refused("origin2000-rack", swap,
                      "row 2 must add node boards to the previous row's")

    def test_a_slot_goes_to_a_node_on_its_own_crossbow(self):
        def cross(rows):
            rows[1]["xio_slots"]["xio-slot-7"] = 1
        self._refused("origin2000-rack", cross,
                      "gives XIO slot 'xio-slot-7' to node slot 1, whose Hub "
                      "is on another Crossbow")

    def test_a_population_beyond_two_per_node_is_refused(self):
        def three(rows):
            rows[0]["supported_cpu_populations"].append([3])
        self._refused("origin2000-rack", three, "wired for 1 to 2")

    def test_the_largest_row_fits_every_node_board(self):
        self._refused("origin2000-rack", lambda rows: rows.pop(),
                      "largest node-population row must fit every node board")

    def test_a_row_without_a_grade_is_refused(self):
        def ungraded(rows):
            del rows[0]["grade"]
        self._refused("origin2000-rack", ungraded, "row 0 has no evidence grade")

    def test_a_board_whose_controller_a_row_leaves_out_is_refused(self):
        # A board fitted in IO4, which node slot 3 controls in the chassis:
        # the one-node row cannot fit it.
        model = _resolved("origin2000-rack")
        for selection in model["composition"]["connector_population"]:
            if selection["component"] == "xio-slot-4":
                selection["occupancy"] = "occupied"
                selection["occupant"] = "io4-board"
        model["components"] += [
            {"id": "io4-board", "name": "Fitted board", "kind": "board",
             "hardware_kind": "pci-shoehorn", "parent": "xio-slot-4",
             "physical_slot": {"kind": "io", "number": 4},
             "identity": {"part": "030-1275-002"}, "provenance": "derived"},
            {"id": "io4-bridge", "name": "Bridge", "kind": "pci-host-bridge",
             "hardware_kind": "bridge", "pci_host_bridge": {"heads": 1},
             "identity": {"widget_part": "0xc002", "widget_mfg": "0x36",
                          "widget_revision": 4},
             "parent": "io4-board", "provenance": "derived"},
        ]
        model["connections"].append(
            {"component": "io4-bridge", "bus": "xbow0-io4",
             "role": "bridge-upstream", "provenance": "derived"})
        model["machine"]["emulation"]["synthetic_identities"].append(
            {"component": "io4-bridge", "serial": "0x51454d55420d",
             "reason": "fixture"})
        for row in model["composition"]["topology"]["node_populations"]:
            row["xio_slots"].pop("xio-slot-4")
        with self.assertRaisesRegex(
                CompileError, "row 0 leaves out node slot 3, which controls "
                "one of the chassis's own I/O boards"):
            compile_model(model)


# Each catalogued configuration captures its compiled JSON and manifest,
# or its refusal message, under GOLDEN_ROOT. ORIGAMI_UPDATE_GOLDENS=1
# recaptures the golden files.
CONFIGURATIONS = (
    "origin200", "origin200-dual", "origin200-gigachannel",
    "origin200-gigachannel-loopback",
    "origin2000", "origin2000-rack", "origin2000-deskside",
    "onyx2-rack", "onyx2-deskside",
    "origin300", "origin300-two-chassis",
)


class GoldenTests(unittest.TestCase):
    def test_every_configuration_matches_its_captured_outcome(self):
        update = os.environ.get("ORIGAMI_UPDATE_GOLDENS") == "1"
        for name in CONFIGURATIONS:
            with self.subTest(configuration=name):
                directory = GOLDEN_ROOT / name
                model = _resolved(name)
                try:
                    catalogue = render_catalogue_json([model])
                except CompileError as exc:
                    expected = {"refused.txt": str(exc) + "\n"}
                else:
                    manifest = render_manifest(
                        {name: model}, {"sgi-machines.json": catalogue})
                    expected = {"sn-model.json": manifest,
                                "sgi-machines.json": catalogue}
                if update:
                    directory.mkdir(parents=True, exist_ok=True)
                    for stale in directory.iterdir():
                        if stale.name not in expected:
                            stale.unlink()
                    for filename, content in expected.items():
                        (directory / filename).write_text(content)
                    continue
                self.assertTrue(
                    directory.is_dir(),
                    f"missing golden directory {directory}; run with "
                    "ORIGAMI_UPDATE_GOLDENS=1 to capture it",
                )
                existing = {path.name for path in directory.iterdir()}
                self.assertEqual(existing, set(expected), f"{name} golden file set changed")
                for filename, content in expected.items():
                    self.assertEqual(
                        (directory / filename).read_text(), content,
                        f"{name}/{filename} does not match its golden",
                    )


class Origin300RoutedContractTests(unittest.TestCase):
    def model(self):
        return _resolved("origin300-numalink-pci-expansion")

    def test_selected_router_and_cables(self):
        topology = compile_model(self.model())
        self.assertEqual(topology["routers"][0]["part"], 0x3017)
        self.assertEqual(topology["routers"][0]["port_count"], 8)
        self.assertEqual(topology["routers"][0]["revision"], 1)
        self.assertEqual(len(topology["nodes"]), 2)
        self.assertEqual(len(topology["io_peers"]), 1)
        links = topology["links"]
        self.assertEqual(len(links), 3)
        self.assertEqual({end["port"] for link in links for end in link["ends"]
                          if end["kind"] == "ROUTER"}, {1, 4, 6})
        self.assertEqual(
            len({c["module_index"] for c in topology["controllers"]}), 4)

    def test_connector_annotations_do_not_hide_extra_link_endpoints(self):
        from sgi_model.model_validation import check_widget_and_pci_uniqueness
        model = self.model()
        components = {c["id"]: c for c in model["components"]}
        buses = {b["id"]: b for b in model["buses"]}
        self.assertFalse(
            check_widget_and_pci_uniqueness(model, components, buses))
        model["connections"].append({"component": "router-port-8",
                                     "bus": "master-route", "role": "endpoint"})
        problems = check_widget_and_pci_uniqueness(model, components, buses)
        self.assertTrue(any("master-route" in p and "exactly two ports" in p
                            for p in problems))

    def test_cable_cannot_annotate_more_than_two_connector_ends(self):
        from sgi_model.model_validation import check_widget_and_pci_uniqueness
        model = self.model()
        for cid, peer in [("router-port-7", "router-port-8"),
                          ("router-port-8", "router-port-7")]:
            selection = next(
                s for s in model["composition"]["connector_population"]
                if s["component"] == cid)
            selection.update(occupancy="cabled", peer=peer)
            model["connections"].append({
                "component": cid, "bus": "master-route", "role": "endpoint"})
        components = {c["id"]: c for c in model["components"]}
        buses = {b["id"]: b for b in model["buses"]}
        problems = check_widget_and_pci_uniqueness(model, components, buses)
        self.assertTrue(any("master-route" in p and "exactly two ports" in p
                            for p in problems))

    def test_router_family_and_port_count_must_agree(self):
        for part, ports in [(0xc002, 8), (0x3017, 6)]:
            with self.subTest(part=part, ports=ports):
                model = self.model()
                router = next(
                    c for c in model["components"]
                    if c["id"] == "router-router")
                router["identity"]["part"] = part
                router["router_asic"]["port_count"] = ports
                with self.assertRaisesRegex(
                        CompileError, "eight ports and part"):
                    compile_model(model)

    def test_router_link_port_bounds_and_reuse(self):
        for port, error in [(0, "out of range"), (9, "out of range"),
                            (6, "attached twice")]:
            with self.subTest(port=port):
                model = self.model()
                connection = next(c for c in model["connections"]
                                  if c["component"] == "router-router"
                                  and c["bus"] == "master-route")
                connection["fabric_port"] = port
                with self.assertRaisesRegex(CompileError, error):
                    compile_model(model)

    def test_expansion_refuses_base_only_router_ports(self):
        for port in [1, 6, 7, 8]:
            with self.subTest(port=port):
                model = self.model()
                connection = next(c for c in model["connections"]
                                  if c["component"] == "router-router"
                                  and c["bus"] == "expansion-cable")
                connection["fabric_port"] = port
                with self.assertRaisesRegex(CompileError, "ports 2-5"):
                    compile_model(model)

    def test_malformed_connector_ids_and_peers_are_refused(self):
        from sgi_model.model_composition import validate_connector_population
        from sgi_model.exceptions import ResolutionError
        for field in ["component", "peer"]:
            for value in [[], {}]:
                with self.subTest(field=field, value=value):
                    model = self.model()
                    selection = next(
                        s for s in model["composition"]["connector_population"]
                        if s["component"] == "master-link-connector")
                    selection[field] = value
                    with self.assertRaises(ResolutionError):
                        validate_connector_population(model)

    def test_cable_peer_requires_reciprocal_selection(self):
        from sgi_model.model_composition import validate_connector_population
        from sgi_model.exceptions import ResolutionError
        model = self.model()
        selection = next(s for s in model["composition"]["connector_population"]
                         if s["component"] == "master-link-connector")
        selection["peer"] = "router-port-8"
        with self.assertRaisesRegex(ResolutionError, "reciprocal"):
            validate_connector_population(model)

    def test_cable_peer_requires_same_cable_bus(self):
        from sgi_model.model_composition import validate_connector_population
        from sgi_model.exceptions import ResolutionError
        model = self.model()
        replacements = {"master-link-connector": "router-port-6",
                        "router-port-6": "master-link-connector",
                        "slave-link-connector": "router-port-1",
                        "router-port-1": "slave-link-connector"}
        for selection in model["composition"]["connector_population"]:
            if selection["component"] in replacements:
                selection["peer"] = replacements[selection["component"]]
        with self.assertRaisesRegex(ResolutionError, "share no cable bus"):
            validate_connector_population(model)

    def test_cable_peer_cannot_replace_a_board_occupant(self):
        from sgi_model.model_composition import validate_connector_population
        from sgi_model.exceptions import ResolutionError
        model = self.model()
        selection = next(s for s in model["composition"]["connector_population"]
                         if s["component"] == "master-link-connector")
        selection["occupant"] = "slave-module"
        with self.assertRaisesRegex(ResolutionError, "reciprocal"):
            validate_connector_population(model)


class L1PeerLinkTests(unittest.TestCase):
    @staticmethod
    def pair_links():
        return [{"kind": "l1-peer", "source": "master-l1",
                 "target": "slave-l1"},
                {"kind": "l1-peer", "source": "slave-l1",
                 "target": "master-l1"}]

    def test_pair_compiles_and_emits_explicit_peers(self):
        from sgi_model.model_emit import _render_topology
        model = _resolved("origin300-two-chassis")
        model["links"] = self.pair_links()
        entry = compile_model(model)
        self.assertEqual(entry.get("l1_peer_links"), [
            {"source_controller_index": 0, "target_controller_index": 1},
            {"source_controller_index": 1, "target_controller_index": 0}])
        emitted = _render_topology(entry, "origin300")
        self.assertEqual(emitted.get("l1-peer-links"), [
            {"source-controller-index": 0, "target-controller-index": 1},
            {"source-controller-index": 1, "target-controller-index": 0}])
        model["components"].reverse()
        self.assertEqual(compile_model(model)["l1_peer_links"],
                         entry["l1_peer_links"])

    def test_peer_kinds_and_degrees_are_checked_by_composition(self):
        from sgi_model.model_composition import validate_object_links
        from sgi_model.exceptions import ResolutionError
        components = [{"id": ident, "kind": "system-controller",
                       "system_controller": {"kind": "l1"}}
                      for ident in ("a", "b", "c")]
        links = [{"kind": "l1-peer", "source": "a", "target": "b"},
                 {"kind": "l1-peer", "source": "b", "target": "a"}]
        model = {"components": components, "links": links}
        self.assertEqual(validate_object_links(model), set())
        links.append({"kind": "l1-peer", "source": "a", "target": "c"})
        with self.assertRaisesRegex(ResolutionError, "competing"):
            validate_object_links(model)
        links.pop()
        components[1]["system_controller"]["kind"] = "msc"
        with self.assertRaisesRegex(ResolutionError, "must be an L1"):
            validate_object_links(model)
        components[1]["system_controller"]["kind"] = "l1"
        links[0]["extra"] = True
        with self.assertRaisesRegex(ResolutionError, "only kind"):
            validate_object_links(model)

    def test_missing_links_do_not_follow_fabric(self):
        model = _resolved("origin300-two-chassis")
        model.pop("links", None)
        self.assertNotIn("l1_peer_links", compile_model(model))

    def test_invalid_peer_links_are_rejected(self):
        for change, message in [
            (lambda links: links.pop(), "reciprocal"),
            (lambda links: links.append(copy.deepcopy(links[0])), "duplicate"),
            (lambda links: links[0].update(target="master-l1"), "itself"),
            (lambda links: links[0].update(target="missing"), "dangling"),
            (lambda links: links[0].update(kind="unknown"), "unsupported kind"),
        ]:
            model = _resolved("origin300-two-chassis")
            model["links"] = self.pair_links()
            change(model["links"])
            with self.subTest(message=message), self.assertRaisesRegex(
                    CompileError, message):
                compile_model(model)


# The I-brick's XBridge and IO7, which the compiler does not yet compile.
_IBRICK_IO = {"ibrick-xbridge", "ibrick-fibre-channel", "ibrick-ioc3",
              "ibrick-usb", "ibrick-firewire"}


def _origin3200_fixture(*, ibrick_io: bool = False) -> dict:
    """The Origin 3200 composition made compilable for compiler tests.

    The catalogue entry stays a reference until the C-brick layer supplies
    its memory, processors and L1, so this borrows the two-chassis Origin
    300's emulation choices, processor, L1 and synthetic Bedrock serials
    to reach the code under test. None of them is an Origin 3200 claim.
    Without `ibrick_io` the I-brick keeps only its crossbar, with a
    placeholder revision, since its PCI heads are not compiled yet.
    """
    donor = _resolved("origin300-two-chassis")
    model = _resolved("origin3200")
    emulation = copy.deepcopy(donor["machine"]["emulation"])
    for key in ("io_boards", "system_timekeepers", "initial_module_numbers",
                "initially_operational_crossbow_links"):
        emulation.pop(key, None)
    rename = {"master-": "c0-", "slave-": "c1-"}

    def renamed(ident):
        for old, new in rename.items():
            if ident.startswith(old):
                return new + ident[len(old):]
        return ident

    for section in ("bedrock_nodes", "l1_controllers"):
        emulation[section] = {renamed(key): value
                              for key, value in emulation[section].items()}
    emulation["synthetic_identities"] = [
        dict(entry, component=renamed(entry["component"]))
        for entry in emulation["synthetic_identities"]
        if entry["component"].endswith("-bedrock")]
    model["machine"]["model_role"] = "emulation-target"
    model["machine"]["emulation"] = emulation
    model["processors"] = copy.deepcopy(donor["processors"])
    components = {item["id"]: item for item in donor["components"]}
    for index, component in enumerate(model["components"]):
        if component["kind"] == "cpu":
            model["components"][index] = dict(
                copy.deepcopy(components["master-cpu0"]),
                id=component["id"], parent=component["parent"],
                processor_slice="AB"[int(component["id"][-1])])
    for brick in ("c0", "c1"):
        model["components"].append(dict(
            copy.deepcopy(components["master-l1"]), id=f"{brick}-l1",
            parent=f"{brick}-chassis"))
    model["composition"]["population"]["memory_bank_mib_per_node"] = [
        [512, 0, 0, 0, 0, 0, 0, 0], [512, 0, 0, 0, 0, 0, 0, 0]]
    if not ibrick_io:
        model["components"] = [item for item in model["components"]
                               if item["id"] not in _IBRICK_IO]
        model["connections"] = [item for item in model["connections"]
                                if item["component"] not in _IBRICK_IO]
        model["buses"] = [item for item in model["buses"]
                          if item["id"] != "ibrick-io7-bus"]
        for component in model["components"]:
            if component["id"] == "ibrick-xbridge-crossbar":
                component["parent"] = "ibrick-brick"
                component["identity"] = {"widget_revision": 1}
    return model


def _component(model: dict, ident: str) -> dict:
    return next(item for item in model["components"] if item["id"] == ident)


class XioCableCompileTests(unittest.TestCase):
    def test_cable_joins_the_node_to_its_enclosure_crossbar(self):
        from sgi_model.model_emit import _render_topology
        entry = compile_model(_origin3200_fixture())
        self.assertEqual(entry["xio_cables"], [{
            "name": "c0-to-ibrick",
            "ends": [{"kind": "NODE", "index": 0},
                     {"kind": "CROSSBAR", "index": 0, "port": 0xa}]}])
        self.assertEqual(entry["crossbars"], [{
            "id": "ibrick-xbridge-crossbar", "module_index": 2,
            "kind": "XBRIDGE", "widget_revision": 1}])
        self.assertEqual([node["board_kind"] for node in entry["nodes"]],
                         ["ip35", "ip35"])
        self.assertEqual(entry["io_boards"], [])
        self.assertEqual(entry["xio_attachments"], [])
        emitted = _render_topology(entry, "origin3200")
        self.assertEqual(emitted["xio-cables"], [{
            "name": "c0-to-ibrick",
            "ends": [{"kind": "node", "index": 0},
                     {"kind": "crossbar", "index": 0, "port": 0xa}]}])
        self.assertEqual([node["board-kind"] for node in emitted["nodes"]],
                         ["ip35", "ip35"])

    def test_existing_topologies_carry_no_xio_cables(self):
        for name in ("origin300", "origin300-two-chassis", "fuel"):
            with self.subTest(name=name):
                entry = compile_model(_resolved(name))
                self.assertNotIn("xio_cables", entry)
                self.assertNotEqual(entry["nodes"][0].get("board_kind"),
                                    "ip35")

    def test_enclosure_crossbar_needs_its_fitted_revision(self):
        model = _origin3200_fixture()
        del _component(model, "ibrick-xbridge-crossbar")["identity"]
        with self.assertRaisesRegex(
                CompileError,
                "ibrick-xbridge-crossbar has no widget_revision"):
            compile_model(model)

    def test_ip35_node_has_no_onboard_crossbar(self):
        model = _origin3200_fixture()
        model["xio_cables"] = []
        with self.assertRaisesRegex(
                CompileError,
                "IP35 node 'c0-bedrock' has crossbar "
                "'ibrick-xbridge-crossbar' on its II with no XIO cable"):
            compile_model(model)

    def test_onboard_xbridge_node_takes_no_xio_cable(self):
        model = _origin3200_fixture()
        model["machine"]["board_ip"] = "IP45"
        with self.assertRaisesRegex(
                CompileError,
                "node 'c0-bedrock' is not an IP35 node; only an IP35 node's "
                "II, with no XBridge on its board, takes an XIO cable"):
            compile_model(model)

    def test_enclosure_attachment_names_the_node_its_cable_reaches(self):
        for node, message in (
                ("c1-bedrock", "names node 'c1-bedrock', which no XIO cable "
                 "joins to it"),
                ("c0-bedrock", "XIO attachments on enclosure crossbar "
                 "'ibrick-xbridge-crossbar' are not compiled yet")):
            with self.subTest(node=node):
                model = _origin3200_fixture()
                connection = next(item for item in model["connections"]
                                  if item["component"] == "ibrick-port-b")
                connection.update(widget=11, controlled_by=node)
                with self.assertRaisesRegex(CompileError, message):
                    compile_model(model)

    def test_pci_heads_on_an_enclosure_crossbar_are_not_compiled_yet(self):
        with self.assertRaisesRegex(
                CompileError,
                "I/O board 'ibrick-xbridge' carries enclosure crossbar "
                "'ibrick-xbridge-crossbar'; PCI heads on an enclosure "
                "crossbar are not compiled yet"):
            compile_model(_origin3200_fixture(ibrick_io=True))


class ModuleRecordCompileTests(unittest.TestCase):
    def test_module_records_carry_brick_type_and_absent_controller(self):
        from sgi_model.model_emit import _render_topology
        model = _origin3200_fixture()
        _component(model, "ibrick-brick")["controller"] = "absent"
        entry = compile_model(model)
        modules = {module["id"]: module for module in entry["modules"]}
        self.assertEqual(modules["c0-chassis"]["brick_type"], "C")
        self.assertEqual(modules["c1-chassis"]["brick_type"], "C")
        self.assertNotIn("controller", modules["c0-chassis"])
        self.assertEqual(modules["ibrick-brick"]["controller"], "absent")
        self.assertEqual(modules["ibrick-brick"]["controller_index"], -1)
        self.assertNotIn("brick_type", modules["ibrick-brick"])
        emitted = {module["id"]: module for module in
                   _render_topology(entry, "origin3200")["modules"]}
        self.assertEqual(emitted["c0-chassis"]["brick-type"], "C")
        self.assertEqual(emitted["ibrick-brick"]["controller"], "absent")

    def test_existing_modules_carry_neither_field(self):
        for name in ("origin300-two-chassis", "origin200", "origin2000-rack"):
            with self.subTest(name=name):
                for module in compile_model(_resolved(name))["modules"]:
                    self.assertNotIn("brick_type", module)
                    self.assertNotIn("controller", module)

    def test_absent_controller_needs_an_empty_module(self):
        model = _origin3200_fixture()
        _component(model, "c0-chassis")["controller"] = "absent"
        with self.assertRaisesRegex(
                CompileError,
                "module 'c0-chassis' has controller: absent but carries a "
                "node or I/O peer"):
            compile_model(model)

    def test_absent_controller_has_no_controller_index(self):
        model = _origin3200_fixture()
        _component(model, "ibrick-brick")["controller"] = "absent"
        model["components"].append(dict(
            copy.deepcopy(_component(model, "c0-l1")), id="ibrick-l1",
            parent="ibrick-brick"))
        with self.assertRaisesRegex(
                CompileError,
                "module 'ibrick-brick' has controller: absent but controller "
                "index 2"):
            compile_model(model)


class NodeRoleCompileTests(unittest.TestCase):
    def test_memory_node_is_never_the_console(self):
        model = _origin3200_fixture()
        _component(model, "c0-bedrock")["node_asic"]["role"] = "memory"
        with self.assertRaisesRegex(
                CompileError, "memory node 'c0-bedrock' cannot be the "
                "console node"):
            compile_model(model)

    def test_memory_node_needs_a_board_allowing_no_processors(self):
        model = _origin3200_fixture()
        _component(model, "c1-bedrock")["node_asic"]["role"] = "memory"
        with self.assertRaisesRegex(
                CompileError, "memory node 'c1-bedrock': board layer 'ip35' "
                "does not allow a node with no processors"):
            compile_model(model)

    def test_compute_node_needs_memory(self):
        model = _origin3200_fixture()
        model["composition"]["population"]["memory_bank_mib_per_node"][1] = [
            0] * 8
        with self.assertRaisesRegex(
                CompileError, "node 'c1-bedrock' has processors and no "
                "memory"):
            compile_model(model)

    def test_compute_role_is_the_default_and_not_emitted(self):
        entry = compile_model(_origin3200_fixture())
        self.assertTrue(all("role" not in node for node in entry["nodes"]))


if __name__ == "__main__":
    unittest.main()
