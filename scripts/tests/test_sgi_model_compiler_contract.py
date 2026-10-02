# SPDX-License-Identifier: BSD-3-Clause

#!/usr/bin/env python3
"""Tests for the format-2 compiler and the compiled-catalogue emitter.

`model_compile.compile_model` is additive: today's three exporters and
`tools/origami model qemu-inputs` are untouched (the format-2 cut-over
replaces them once the QEMU one-builder step lands). These tests exercise
the new modules directly.

Two kinds of coverage:

- Record-shape tests read a real resolved fixture's typed graph index
  (`model_compile.index_graph`) and check one record type's compiled shape
  against values in that fixture, independent of whether the whole
  configuration compiles end to end -- the Origin 2000 family's routers,
  crossbars and modules are exercised this way, since `compile_model`
  does not yet wire crossbar-attached configurations all the way through
  (see the module-level refusal test below).
- Golden tests capture, per catalogued configuration, either the
  compiled-catalogue JSON and its manifest or the specific refusal
  message, under `system-model-fixtures/format2/<configuration>/`.
  ORIGAMI_UPDATE_GOLDENS=1 recaptures them.

The Origin 200 configuration's compiled node, its processor, its flash and
its I/O board are additionally checked field by field against the values
QEMU's `hw/mips/sgi/sn-model.c` (branch `sn/one-builder-step`) hand-
transcribes from today's generated inputs -- the plan's format-2 section
names that file the reference for exactly this configuration. Those checks
read `compile_model`'s own untrimmed dict, not the catalogue JSON: the
ownership rule the catalogue applies (`CatalogueJsonTests`, below) is a
property of `render_catalogue_json`'s output, not of what the compiler
itself resolves.
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

from sgi_model.model_compile import CompileError, compile_model, index_graph
from sgi_model.model_emit import render_catalogue_json as _render_catalogue_json, render_manifest
from sgi_model.model_validation import load_checked_system_model, schema
from sgi_model.registry import PRODUCT_MODELS


# Every class below reads a handful of the same six real docs/machines
# files, most read fresh and unmutated; one test point
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

    def test_fuel_unknowns_preserve_required_and_optional_inputs(self):
        topology = self.catalogue(_resolved("fuel"))
        node = topology["nodes"][0]
        self.assertNotIn("widget-revision", node["identity"])
        self.assertNotIn("board-id-word", node["bedrock"])
        self.assertEqual(node["bedrock"]["installed-bank-mask"], 3)
        self.assertEqual(node["bedrock"]["board-id-input"], {
            "option": "fuel-board-id-word", "required": True})
        l1 = topology["controllers"][0]["l1"]
        self.assertNotIn("reported-type", l1)
        self.assertNotIn("revision", l1)
        self.assertEqual(l1["reported-type-input"], {
            "option": "fuel-l1-type-code", "required": True})
        self.assertEqual(l1["revision-input"], {
            "option": "fuel-l1-revision", "required": False})
        board = topology["io-boards"][0]
        self.assertTrue(board["populated"])
        self.assertEqual(board["mac-source"], "module-l1")
        self.assertNotIn("ioc3-subsystem-id", board)
        self.assertEqual(board["ioc3-subsystem-input"], {
            "option": "fuel-ioc3-subsystem-id", "required": True})
        self.assertEqual(board["pci-functions"], [
            {"kind": "isp12160", "revision": 6},
            {"kind": "ioc3", "revision": 1},
            {"kind": "lucent-ohci", "revision": 16},
        ])

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

    def test_default_and_required_binding_are_distinct(self):
        model = _resolved("origin300")
        model["machine"]["emulation"]["bedrock_nodes"]["bedrock"][
            "board_id_input"] = {
                "option": "fuel-board-id-word", "required": False}
        config = self.catalogue(model)["nodes"][0]["bedrock"]
        self.assertEqual(config["board-id-word"], 0x8001)
        self.assertFalse(config["board-id-input"]["required"])

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

    def test_unused_l1_cannot_accept_reported_configuration(self):
        model = _resolved("origin300-vbrick")
        model["machine"]["emulation"]["l1_controllers"]["graphics-l1"] = {
            "reported_type": 0x34,
            "revision_input": {"option": "fuel-l1-revision", "required": False},
        }
        with self.assertRaisesRegex(ValueError, "no Bedrock"):
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

    def test_incomplete_required_values_are_rejected(self):
        for section, owner, field in (
                ("bedrock_nodes", "bedrock", "board_id_word"),
                ("l1_controllers", "l1", "reported_type")):
            with self.subTest(field=field):
                model = _resolved("origin300")
                del model["machine"]["emulation"][section][owner][field]
                with self.assertRaisesRegex(ValueError, "required input"):
                    compile_model(model)
        model = _resolved("origin300")
        model["machine"]["emulation"].pop("l1_controllers")
        with self.assertRaisesRegex(ValueError, "configured L1"):
            compile_model(model)
        model = _resolved("fuel")
        model["machine"]["emulation"]["bedrock_nodes"]["bedrock"][
            "revision_input"]["required"] = False
        with self.assertRaisesRegex(ValueError, "Bedrock revision"):
            compile_model(model)

    def test_binding_aliases_have_one_owner(self):
        model = _resolved("origin300-two-chassis")
        for entry in model["machine"]["emulation"]["l1_controllers"].values():
            entry["revision_input"] = {
                "option": "fuel-l1-revision", "required": False}
        with self.assertRaisesRegex(ValueError, "multiple owners"):
            compile_model(model)

    def test_offering_inputs_follow_selected_configuration(self):
        model = _resolved("fuel")
        policy = model["machine"]["emulation"]["bedrock_nodes"]["bedrock"]
        policy["board_id_word"] = 0x4000
        policy["board_id_input"]["required"] = False
        catalogue = json.loads(
            render_catalogue_json([model, _resolved("origin300")]))
        offerings = catalogue["offerings"]
        fuel = next(item for item in offerings if item["topology"] == "fuel")
        self.assertEqual(len(fuel["hardware-inputs"]), 6)
        board = next(binding for binding in fuel["hardware-inputs"]
                     if binding["option"] == "fuel-board-id-word")
        self.assertFalse(board["required"])
        for offering in offerings:
            if offering["topology"] == "origin300":
                self.assertEqual(offering["hardware-inputs"], [])


class RecordShapeTests(unittest.TestCase):
    """Compile shape per record type, read off real resolved fixtures."""

    def test_synthetic_expansion_pending_selection(self):
        model = _resolved(
            "origin300-pci-expansion-unconnected-uart-experiment")
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
            "origin300-pci-expansion-configuration-storage-experiment")
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
             "datasheet_contracts": ["docs/datasheets/bedrock-network-interface.md"]},
        ])
        model["machine"]["emulation"].setdefault("synthetic_identities", []).append({
            "component": "io-bedrock", "kind": "bedrock", "serial": "0x123456",
        })
        entry = compile_model(model)
        self.assertEqual(len(entry["nodes"]), 1)
        self.assertEqual(entry["io_peers"], [{
            "id": "io-bedrock", "module_index": 1, "slot": 0,
            "asic_kind": "BEDROCK", "identity": {
                "ni_revision": 1, "widget_revision": 1, "serial": 0x123456,
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
             "datasheet_contracts": ["docs/datasheets/bedrock-network-interface.md"]},
        ])
        model["machine"]["emulation"].setdefault("synthetic_identities", []).append({
            "component": "io-bedrock", "kind": "bedrock", "serial": "0x123456",
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
        model = _resolved("origin300-pci-expansion-experiment")
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
        model = _resolved("origin300-pci-expansion-experiment")
        raw = model["machine"]["emulation"]["io_assemblies"][0]
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
        model = _resolved("origin300-pci-expansion-experiment")
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
            "origin300-pci-expansion-experiment",
            "origin300-pci-expansion-configuration-storage-experiment",
            "origin300-pci-expansion-unconnected-uart-experiment",
            "origin300-pci-expansion-pending-register-experiment",
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

    def test_origin300_direct_v12_exports_one_odyssey_board(self):
        entry = compile_model(_resolved("origin300-v12-direct"))

        self.assertEqual(entry["name"], "origin300-v12-direct")
        self.assertEqual([(module["id"], module["nodes"])
                          for module in entry["modules"]],
                         [("implicit-module", [0])])
        self.assertEqual([node["module_index"] for node in entry["nodes"]], [0])
        self.assertEqual([(board["id"], board["module_index"],
                           board["board_kind"], board["slot_index"])
                          for board in entry["io_boards"]],
                         [("xbridge-bridge-e", 0, "IO8", 0),
                          ("xbridge-bridge-f", 0, "IO8", 1),
                          ("graphics-v12", 0, "ODYSSEY", 2)])
        self.assertEqual(entry["xio_attachments"][2], {
            "crossbar_index": -1, "widget": 11, "node_index": 0,
        })
        catalogue = json.loads(render_catalogue_json([_resolved("origin300-v12-direct")]))
        board = catalogue["topologies"][0]["io-boards"][2]
        self.assertEqual((board["preset"], board["carrier"], board["module-index"]),
                         ("odyssey", "machine-placed", 0))
        self.assertEqual(board["revision"], 10)

    def test_origin300_vbrick_exports_one_enclosure_preset(self):
        entry = compile_model(_resolved("origin300-vbrick"))

        self.assertEqual([(module["id"], module["nodes"])
                          for module in entry["modules"]],
                         [("implicit-module", [0]), ("graphics-vbrick", [])])
        self.assertEqual([(board["id"], board["module_index"],
                           board["board_kind"]) for board in entry["io_boards"]],
                         [("xbridge-bridge-e", 0, "IO8"),
                          ("xbridge-bridge-f", 0, "IO8"),
                          ("graphics-vbrick", 1, "VBRICK")])
        self.assertEqual(entry["xio_attachments"][2], {
            "crossbar_index": -1, "widget": 11, "node_index": 0,
        })
        self.assertEqual([(controller["id"], controller["module_index"])
                          for controller in entry["controllers"]],
                         [("graphics-l1", 1), ("l1", 0)])
        catalogue = json.loads(render_catalogue_json([_resolved("origin300-vbrick")]))
        board = catalogue["topologies"][0]["io-boards"][2]
        self.assertEqual((board["preset"], board["module-index"],
                          board["attachment"]), ("vbrick", 1, 2))

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
        # docs/datasheets/onyx2.md: the deskside's midplane drives Hub slot
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
        # Crossbow 2 (widgets 10 and 9) -- per
        # docs/machines/layers/modules/origin2000-module.yaml's XIO
        # buses.
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
        # Both routers' serials are the emulator's synthetic identities
        # (docs/machines/layers/products/origin2000-rack.yaml,
        # machine.emulation.synthetic_identities), the same mechanism a
        # node's Hub uses -- never the identity block's own absent serial.
        self.assertEqual(router["serial"], 0x100000000005)
        self.assertEqual(entry["routers"][1]["id"], "router-2")
        self.assertEqual(entry["routers"][1]["serial"], 0x100000000006)
        # A crossbar's manufacturer id, part number and initially-
        # populated-links mask are fixed by which chip it is (decision
        # D-0026) and are not carried here.
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
        # docs/machines/origin200-dual.yaml's peer-craylink fabric bus now
        # authors link.kind (cable); no router or crossbar is involved, so
        # this is the flat Origin 200 node path run twice, not the
        # router/crossbar assembly path.
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
            [0xc0b0a090807, 0xc0b0a090808],
        )
        self.assertEqual(
            [board["serial"] for board in entry["io_boards"]],
            [0x60504030201, 0x60504030202],
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
        # now (decision D-0026) and is not carried here; the IOC3's own
        # fitted-part one-wire serial has no other home and survives.
        entry = compile_model(_resolved("origin2000-rack"))
        board = entry["io_boards"][0]
        self.assertEqual(board["id"], "baseio")
        # This is a real physical board of its own (docs/machines/layers/
        # modules/origin2000-module.yaml's "baseio" board component, IO6
        # slot 1), the Origin 2000 family's BaseIO. The default origin2000
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
    """Fit a one-Bridge board into origin2000-rack's IO2 connector.

    The resolved-graph shape a composition produces when it instantiates
    a board module under the connector and selects the connector occupied
    (docs/designs/2026-09-05-origin2000-generated-descriptor.md, "A fitted
    board in an occupied connector"). Built in memory so the compiler is
    exercised independently of any one composition file.
    """
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
        {"component": "io2-bridge", "serial": "0x10000000000d",
         "reason": "fixture"},
        {"component": "io2-ioc3", "serial": "0x10000000000e", "reason": "fixture"},
    ]
    return model


def _fit_widget_occupant(model: dict, *, hardware_kind: str = "xg",
                         board_kind: str = "kona", widget_revision: int = 0) -> dict:
    """Fit a bridge-less XIO widget board into origin2000-rack's IO2
    connector: XG, the InfiniteReality pipe's XIO-to-graphics ASIC
    (`_XIO_WIDGET_KINDS`), attached to the connector's XIO bus
    directly with role endpoint, carrying no PCI host bridge and no PCI
    bus of its own (docs/machines/layers/modules/kona-pipe.yaml's own
    xg connection uses this same role for this same reason). Built in
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
        {"component": "io2-widget", "serial": "0x10000000000d", "reason": "fixture"})
    return model


def _fit_widget_occupant_one_wire_chain(model: dict) -> dict:
    """Extend `_fit_widget_occupant`'s XG-style widget with a one-wire
    identity chain answering for two boards (the Onyx2 rack's pipe shape,
    docs/machines/layers/modules/kona-pipe.yaml and
    ir-graphics-module.yaml): a sub-board fitted on the widget's own
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
        self.assertEqual(board["serial"], 0x10000000000d)
        self.assertEqual(board["ioc3_serial"], 0x10000000000e)
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
        self.assertEqual(board["serial"], 0x10000000000d)
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
        nvram = next(item for item in offering["resources"] if item["kind"] == "nvram")
        self.assertEqual(nvram["count"], 1)


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
        nvram = next(item for item in catalogue["offerings"][0]["resources"]
                     if item["kind"] == "nvram")
        self.assertEqual(nvram["count"], 1)

    def test_gigachannel_cabinet_is_a_machine_placed_board(self):
        model = _fit_occupant(_resolved("origin2000-rack"),
                              board_kind="gigachannel-midplane")
        board = json.loads(render_catalogue_json([model]))["topologies"][0]["io-boards"][1]
        self.assertEqual((board["preset"], board["carrier"]),
                         ("gigachannel", "machine-placed"))


class OriginTwoHundredMatchesTheHandWrittenEntryTests(unittest.TestCase):
    """Field values QEMU's hand-transcribed origin200 entry names.

    Transcribed from `hw/mips/sgi/sn-model.c` on the QEMU branch
    `sn/one-builder-step`, commit `7632a48bbf` ("hand-author origin200's
    format-2 entry"). Two fields the compiled entry cannot reproduce --
    the node and module C identifiers "node0"/"chassis", names not
    derived from any authored fact in the resolved model -- are the
    "where it does not match and why" the packet's report names; every
    field with hardware or emulator-identity meaning matches.
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
            "ni_revision": 3, "widget_revision": 1, "serial": 0xc0b0a090807,
            "i2c_controller": True, "system_controller": True,
        })

    def test_processor(self):
        assembly = self.entry["nodes"][0]["cpu_slots"][0]["assembly"]
        self.assertEqual(assembly, {
            "id": "r10000-180mhz", "kind": "R10000",
            "prid": 0x926, "fpu_prid": 0x900,
            "clock_hz": 0xaba9500, "cache_clock_hz": 0x55d4a80,
            "cache_mib": 1, "mode_bits": 0x1211a785,
        })
        self.assertIs(
            self.entry["nodes"][0]["cpu_slots"][1]["assembly"], assembly,
            "both interface slots wire the same catalogued part",
        )

    def test_bank_and_accepted_memory(self):
        node = self.entry["nodes"][0]
        self.assertEqual(node["bank_mib"], [64, 0, 0, 0, 0, 0, 0, 0])
        self.assertEqual(node["accepted_memory_mib"], [512, 256, 128, 64])

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
        self.assertEqual(board["serial"], 0x60504030201)
        self.assertEqual(board["part"], "030-1244-001")
        # A PCI function's own device number, revision and interrupt
        # input are sgi-baseio's own preset facts now (decision D-0026);
        # only the kind order survives, for the catalogue's offerings.
        self.assertEqual(
            [item["kind"] for item in board["pci"]], ["ISP1040", "ISP1040", "IOC3"])
        self.assertEqual(board["ioc3_serial"], 0x5e7000000001)

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
    ni_revision and the flash kind are sourced approximations flagged for
    independent review, not values today's generated input carries at all
    (it has neither field). Decision D-0026 later widened this list: a PCI
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
        # The catalogue wires the maximum population any accepted offering
        # names (docs/machines/layers/products/origin300.yaml's
        # supported_cpu_populations is now [[2], [4]]), so all four
        # processor-interface slots carry a fitted assembly; -smp 2 versus
        # -smp 4 is a QEMU builder selection over this one wiring, not a
        # second compiled topology.
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
        # The reviewed placement (docs/reviews/bedrock-fitted-bank-
        # presence-contract-review-20260912.md): the shipped 512 MiB sits
        # at locational banks two and three, not the dense bank zero
        # today's generated input still carries.
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
        # board, as the L1 and the boot flash are (decision D-0026's own
        # corner case): the node device creates it directly, so it never
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
        # and IOC3 population flags are sgi-baseio's own preset facts now
        # (decision D-0026); only the kind order survives.
        head_f = boards["xbridge-bridge-f"]
        self.assertEqual(
            [item["kind"] for item in head_f["pci"]], ["ISP12160", "IOC3", "LUCENT_OHCI"])
        self.assertEqual(head_f["ioc3_serial"], 0x5e7000000002)

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
    """docs/machines/origin300-two-chassis.yaml: two complete base modules
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
                         [0x0d0c0b0a0908, 0x0d0c0b0a0909])

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
                         [0x0b0a090807, 0x0b0a090807, 0x0b0a090808, 0x0b0a090808])
        self.assertEqual([board["ioc3_serial"] for board in boards],
                         [None, 0x5e7000000002, None, 0x5e7000000003])
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
        flash = [item for item in offerings[0]["resources"] if item["kind"] == "boot-flash"]
        self.assertEqual([item["count"] for item in flash], [2])
        self.assertEqual(
            [(item["kind"], item["node"], item["argument"]) for item in offerings[0]["consoles"]],
            [("console", 0, "-serial chardev:l1"),
             ("system-controller", 1, "-serial chardev:l1_node1")])


class RefusalTests(unittest.TestCase):
    """Reference-role and not-yet-grounded configurations are refused."""

    def test_reference_configuration_is_refused(self):
        # docs/machines/origin2000.yaml (model_role: reference) describes
        # the generic four-node system, not one this compiler ever emits
        # a topology for.
        with self.assertRaisesRegex(CompileError, "not an emulation target"):
            compile_model(_resolved("origin2000"))

    def test_unsupported_generation_is_refused_by_name(self):
        model = _resolved("origin300")
        model["machine"]["generation"] = "SN2"
        with self.assertRaisesRegex(CompileError, "generation 'SN2'"):
            compile_model(model)

    def test_dual_module_compiles_with_router_assembly(self):
        # docs/machines/origin200-dual.yaml's peer-craylink fabric bus is
        # authored with link.kind (cable); the format-2 cut-over adds router
        # and crossbar compilation, so the origin200-dual now compiles
        # successfully instead of being refused.
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

class ManifestTests(unittest.TestCase):
    def test_manifest_format(self):
        manifest_text = render_manifest(
            {"origin200": {"a": 1}}, {"sn-catalogue.json": "{}"})
        manifest = json.loads(manifest_text)
        self.assertEqual(manifest["format"], 4)
        self.assertEqual(manifest["format_key"], "sgi-sn")
        self.assertIn("source_sha256", manifest)
        self.assertIn("sn-catalogue.json", manifest["outputs"])


class CatalogueJsonTests(unittest.TestCase):
    """`render_catalogue_json`'s `topologies` and `offerings` sections,
    checked against independently expected values rather than only a
    captured golden -- the packet's own worked examples.
    """

    def test_document_shape(self):
        catalogue = json.loads(render_catalogue_json([_resolved("origin200")]))
        self.assertEqual(catalogue["schema"], "sgi-sn-catalogue")
        self.assertEqual(catalogue["version"], 1)
        self.assertEqual(set(catalogue), {"schema", "version", "topologies", "offerings"})

    def test_enumerations_are_lower_case_and_hyphenated(self):
        # docs/designs/sn-machine-model-architecture.md's "The compiled
        # catalogue" section: QAPI member names, lower-case enumerations.
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
        # decision D-0026: a topology carries fitted-part identity and
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

    def test_origin_200_has_two_offerings_for_its_accepted_populations(self):
        catalogue = json.loads(render_catalogue_json([_resolved("origin200")]))
        self.assertEqual(
            sorted((o["smp"], o["cpus-per-node"]) for o in catalogue["offerings"]),
            [(1, [1]), (2, [2])],
        )

    def test_origin_200_dual_smp_is_four(self):
        catalogue = json.loads(render_catalogue_json([_resolved("origin200-dual")]))
        offerings = catalogue["offerings"]
        self.assertEqual(len(offerings), 1)
        self.assertEqual(offerings[0]["smp"], 4)
        self.assertEqual(offerings[0]["cpus-per-node"], [2, 2])
        self.assertEqual(offerings[0]["product"], "origin200")

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
            flash = next(r for r in offering["resources"]
                         if r["kind"] == "boot-flash")
            self.assertEqual(flash["count"], offering["nodes"])

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
        # docs/machines/layers/products/origin300.yaml's own
        # topology.supported_cpu_populations names [[2], [4]]; the
        # catalogue must assert exactly those two offerings, sharing the
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
        storage = catalogue["offerings"][0]["storage"]
        self.assertEqual(len(storage), 1)
        self.assertEqual(storage[0]["bus"], "scsi.0")

    def test_origin_200_has_two_storage_controllers(self):
        # Both onboard ISP1040 channels are host adapters of their own.
        catalogue = json.loads(render_catalogue_json([_resolved("origin200")]))
        storage = catalogue["offerings"][0]["storage"]
        self.assertEqual([item["bus"] for item in storage], ["scsi.0", "scsi.1"])

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
            {"component": "io4-bridge", "serial": "0x10000000000d",
             "reason": "fixture"})
        for row in model["composition"]["topology"]["node_populations"]:
            row["xio_slots"].pop("xio-slot-4")
        with self.assertRaisesRegex(
                CompileError, "row 0 leaves out node slot 3, which controls "
                "one of the chassis's own I/O boards"):
            compile_model(model)


# The six catalogued configurations the plan's format-2 section names.
# Each captures either its compiled-catalogue JSON and manifest or its
# refusal message under GOLDEN_ROOT; ORIGAMI_UPDATE_GOLDENS=1 recaptures
# them. The format-2 `sn-model.c` goldens went with `render_sn_model_c`;
# the JSON goldens stay.
CONFIGURATIONS = (
    "origin200", "origin200-dual", "origin200-gigachannel",
    "origin200-gigachannel-loopback",
    "origin2000", "origin2000-rack", "origin2000-deskside",
    "onyx2-rack", "onyx2-deskside",
    "origin300", "origin300-two-chassis", "origin300-v12-direct",
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
                        {name: model}, {"sn-catalogue.json": catalogue})
                    expected = {"sn-model.json": manifest, "sn-catalogue.json": catalogue}
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


if __name__ == "__main__":
    unittest.main()
