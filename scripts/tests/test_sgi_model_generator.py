# SPDX-License-Identifier: BSD-3-Clause

"""Tests for the standalone SGI machine-model generator."""

import copy
import hashlib
import json
import os
import pathlib
import re
import subprocess
import tempfile
import unittest
import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
INPUTS = ROOT / "hw/mips/sgi/models"
EXPECTED_CATALOGUE_SHA256 = (
    "f94406bd61131546ecf9ad27de8575556cb2cf3580a4bb36ac8b0cc9b387c1d6"
)

sys.path.insert(0, str(ROOT / "scripts"))

from sgi_model import model_composition
from sgi_model import model_graphics
from sgi_model.exceptions import ResolutionError
from sgi_model.model_validation import load_checked_system_model, schema
from sgi_model.model_qemu import generate_qemu_inputs, render_qemu_inputs
from sgi_model.model_validation import discover_model_paths, load_checked_system_models
from sgi_model.system_model import _layer
from sgi_model.registry import (
    IMPLEMENTED_TOPOLOGIES, PRODUCT_NAMES, REFERENCE_TOPOLOGIES,
)


class GeneratorTests(unittest.TestCase):
    def test_onyx4_host_serial_profile_names_its_ip53_compatibility(self):
        catalogue = json.loads(
            render_qemu_inputs(INPUTS)["hw/mips/sgi/sgi-machines.json"])
        topology = next(
            item for item in catalogue["topologies"]
            if item["name"] == "onyx4-host-serial-compat")

        self.assertEqual(topology["product"], "onyx4-host")
        self.assertIn("IP53-backed", topology["description"])
        self.assertEqual(len(topology["nodes"]), 1)
        node = topology["nodes"][0]
        self.assertEqual(node["board-kind"], "ip53")
        self.assertEqual(sum(node["bank-mib"]), 8192)
        self.assertEqual(len(node["cpu-slots"]), 4)
        cpu = node["cpu-slots"][0]["assembly"]
        self.assertEqual(cpu["kind"], "R16000")
        self.assertEqual(cpu["clock-hz"], 1000000000)
        self.assertEqual(cpu["cache-mib"], 16)
        self.assertEqual(len(topology["io-boards"]), 1)
        self.assertEqual(topology["io-boards"][0]["id"], "io9")
        self.assertEqual(topology["cables"], [])
        self.assertEqual(topology["routers"], [])
        self.assertEqual(topology["crossbars"], [])

    def test_reference_models_are_public_without_runtime_offerings(self):
        catalogue = json.loads(
            render_qemu_inputs(INPUTS)["hw/mips/sgi/sgi-machines.json"])
        references = catalogue["references"]
        self.assertEqual(
            {item["name"] for item in references}, set(REFERENCE_TOPOLOGIES))
        self.assertTrue(all(item["product"] and item["display-name"]
                            and item["description"] for item in references))
        runtime_names = {item["name"] for item in catalogue["topologies"]}
        runtime_offerings = {item["topology"]
                             for item in catalogue["offerings"]}
        self.assertFalse(set(REFERENCE_TOPOLOGIES) & runtime_names)
        self.assertFalse(set(REFERENCE_TOPOLOGIES) & runtime_offerings)

    def test_ip27_chassis_default_to_the_shipped_256_mib_node_board(self):
        # SGI sold no Origin 2000 or Onyx2 node board under 256 MB
        # (remanufactured Origin 2000 guide 1068 printed page 7, Onyx2
        # guide 1073 printed page 3), so every offering defaults to 256 MiB
        # per node while the 64 MiB DIMM pair stays selectable
        # (007-3439-002 printed page 35).
        catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sgi-machines.json"])
        offerings = [item for item in catalogue["offerings"]
                     if item["product"] in ("origin2000", "onyx2")]
        self.assertTrue(offerings)
        for item in offerings:
            memory = item["memory-per-node-mib"]
            with self.subTest(topology=item["topology"], nodes=item["nodes"]):
                self.assertEqual(memory["default"], 256)
                self.assertIn(64, memory["accepted"])
                self.assertIn(256, memory["accepted"])

    def test_default_export_is_complete_and_compatible(self):
        outputs = render_qemu_inputs(INPUTS)
        catalogue_text = outputs["hw/mips/sgi/sgi-machines.json"]
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
            # One offering per processor option and accepted population
            # its modules make, or per population where there is no choice.
            options = [(option["assembly"]["id"], option["cpus-per-node"])
                       for option in topology.get("processor-options", [])]
            if not options:
                options = [(topology["nodes"][0]["cpu-slots"][0]["assembly"]
                            ["id"], None)]
            for row in topology["node-populations"]:
                if row["grade"] == "unknown":
                    continue
                for population in row["supported-cpu-populations"]:
                    counts = population["cpus-per-node"]
                    for processor, fits in options:
                        if fits is not None and not set(counts) <= set(fits):
                            continue
                        expected_offerings.append((
                            topology["product"], topology["name"],
                            len(row["nodes"]), tuple(counts), processor))
        actual_offerings = [(
            item["product"], item["topology"], item["nodes"],
            tuple(item["cpus-per-node"]), item["processor"]["id"])
            for item in catalogue["offerings"]]
        self.assertEqual(sorted(actual_offerings), sorted(expected_offerings))
        self.assertEqual(manifest["format"], 4)
        self.assertEqual(manifest["format_key"], "sgi-sn")
        self.assertEqual(
            hashlib.sha256(catalogue_text.encode()).hexdigest(),
            EXPECTED_CATALOGUE_SHA256)
        self.assertEqual(
            manifest["outputs"]["hw/mips/sgi/sgi-machines.json"],
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
        resolved.update({
            path.stem: model for path, model in checked
            if path.stem in REFERENCE_TOPOLOGIES
            and model["machine"]["model_role"] == "reference"
        })
        expected_source_digest = hashlib.sha256(json.dumps(
            resolved, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        self.assertEqual(manifest["source_sha256"], expected_source_digest)
        self.assertEqual(render_qemu_inputs(INPUTS), outputs)
        self.assertEqual(set(manifest["outputs"]),
                         {"hw/mips/sgi/sgi-machines.json"})

    def test_cli_rejects_an_incompatible_tree_before_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            tree = pathlib.Path(temp)
            result = subprocess.run(
                [sys.executable, str(ROOT / "scripts/sgi-model.py"),
                 "--inputs", str(INPUTS), "--tree", str(tree)],
                cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cannot read QEMU model format declarations", result.stderr)
            self.assertFalse((tree / "hw/mips/sgi/sgi-machines.json").exists())
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
            self.assertFalse((tree / "hw/mips/sgi/sgi-machines.json").exists())
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
        self.assertEqual(len(list(INPUTS.rglob("*.yaml"))), 139)
        self.assertEqual(len(discover_model_paths(INPUTS)), 43)
        self.assertEqual(len(layers), 94)

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
                {"hw/mips/sgi/sgi-machines.json", "hw/mips/sgi/sn-model.json"})
            files = {str(path.relative_to(tree)) for path in tree.rglob("*")
                     if path.is_file()}
            self.assertEqual(files, {
                "hw/mips/sgi/sgi-machines.json", "hw/mips/sgi/sn-model.json"})
            self.assertFalse((tree / "assets").exists())


class GraphicsPipeTests(unittest.TestCase):
    """The graphics slots, products and refusals each topology lists."""

    @classmethod
    def setUpClass(cls):
        cls.catalogue = json.loads(
            render_qemu_inputs(INPUTS)["hw/mips/sgi/sgi-machines.json"])
        cls.families = model_graphics.load_graphics_products(INPUTS)

    def section(self, products, name="io3", **slot):
        return {"max_pipes": 1, "headless": True, "slots": [
            {"name": name, "description": "XIO slot",
             "placement": {"slot": "{name}"}, **slot,
             "families": [{"family": "mgras", "products": products}]}]}

    def resolve(self, section):
        return model_graphics.resolve_graphics("test", section, self.families)

    def topology(self, name):
        return next(item for item in self.catalogue["topologies"]
                    if item["name"] == name)

    def test_every_record_keeps_the_rules(self):
        seen = {}
        for topology in self.catalogue["topologies"]:
            graphics = topology.get("graphics")
            if graphics is None:
                continue
            self.assertGreaterEqual(graphics["max-pipes"], 1)
            self.assertLessEqual(graphics["max-pipes"],
                                 len(graphics["slots"]))
            self.assertEqual("headless-refused" in graphics,
                             not graphics["headless"])
            names = [slot["name"] for slot in graphics["slots"]]
            self.assertEqual(len(names), len(set(names)))
            texts = [slot["description"] for slot in graphics["slots"]]
            self.assertEqual(len(texts), len(set(texts)))
            defaults = [slot for slot in graphics["slots"] if "default" in slot]
            self.assertLessEqual(len(defaults), 1)
            if not defaults:
                self.assertTrue(graphics["headless"])
            for slot in graphics["slots"]:
                self.assertFalse(slot["changeable"])
                for refusal in slot.get("refusals", []):
                    self.assertFalse(refusal["reason"].endswith("."))
                for family in slot["families"]:
                    for product in family["products"]:
                        key = (family["family"], product["product"])
                        summary = (product["name"], product["description"])
                        self.assertEqual(seen.setdefault(key, summary),
                                         summary)
                        for setting in product["settings"]:
                            self.assertFalse(setting["changeable"])
                            self.assertIn(setting["default"],
                                          [v["value"] for v in
                                           setting["values"]])
                pipe = slot.get("default")
                if pipe is None:
                    continue
                family = next(item for item in slot["families"]
                              if item["family"] == pipe["family"])
                product = next(item for item in family["products"]
                               if item["product"] == pipe["product"])
                values = {item["name"]: item["value"]
                          for item in pipe["settings"]}
                self.assertEqual(sorted(values),
                                 sorted(s["name"] for s in product["settings"]))
                for refusal in slot.get("refusals", []):
                    self.assertFalse(model_graphics.refusal_matches(
                        refusal, pipe["family"], pipe["product"], values))

    def test_unmodelled_hardware_is_absent(self):
        text = json.dumps([topology.get("graphics")
                           for topology in self.catalogue["topologies"]])
        for absent in ('"ir4"', '"reality"', '"v6"', '"v8"', '"v10"',
                       '"rad1"', '"rad4c-km"', "gvo", "dplex"):
            self.assertNotIn(absent, text)

    def test_gigachannel_graphics_is_at_io6(self):
        names = [slot["name"] for slot in
                 self.topology("origin200-gigachannel")["graphics"]["slots"]]
        self.assertEqual(names, ["io6", "pci-5", "pci-6", "pci-7"])

    def test_slots_need_their_node_count(self):
        slots = {slot["name"]: slot for slot in
                 self.topology("origin2000-rack")["graphics"]["slots"]}
        self.assertNotIn("requires", slots["io3"])
        self.assertEqual(slots["io12"]["requires"], {"min-nodes": 2})

    def test_pipe_limits(self):
        limits = {topology["name"]: topology["graphics"]["max-pipes"]
                  for topology in self.catalogue["topologies"]
                  if topology.get("graphics")}
        self.assertEqual(limits.pop("onyx3400-ir"), 4)
        self.assertEqual(limits.pop("onyx2-rack"), 2)
        self.assertEqual(set(limits.values()), {1})

    def test_max_pipes_above_the_slots_is_refused(self):
        section = self.section([{"product": "si", "fidelity": "implemented"}])
        section["max_pipes"] = 2
        section["max_pipes_sources"] = ["a manual"]
        with self.assertRaisesRegex(ResolutionError,
                                    "more than its 1 slots can hold"):
            self.resolve(section)

    def test_max_pipes_above_one_needs_sources(self):
        section = self.many_slots("XIO slot {number}")
        section["max_pipes"] = 2
        with self.assertRaisesRegex(ResolutionError, "max_pipes_sources"):
            self.resolve(section)
        section["max_pipes_sources"] = ["a manual"]
        self.assertEqual(self.resolve(section)["max_pipes"], 2)

    def test_several_pipes_take_at_most_four_slots(self):
        section = self.many_slots("XIO slot {number}")
        section["slots"][0]["names"] = ["io2", "io3", "io4", "io5", "io6"]
        section["max_pipes"] = 2
        section["max_pipes_sources"] = ["a manual"]
        with self.assertRaisesRegex(ResolutionError, "at most 4"):
            self.resolve(section)
        section["slots"][0]["names"] = ["io2", "io3", "io4", "io5"]
        self.resolve(section)

    def test_rack_second_pipe_needs_node_board_2(self):
        slots = {slot["name"]: slot for slot in
                 self.topology("onyx2-rack")["graphics"]["slots"]}
        self.assertNotIn("requires", slots["io3"])
        self.assertEqual(slots["io7"]["requires"], {"min-nodes": 2})
        records = {board["label"]: board for board in
                   self.topology("onyx2-rack")["io-boards"]}
        self.assertEqual(records["io7"]["other-identity-parts"],
                         records["io3"]["other-identity-parts"])

    def pipe_topology(self, *boards):
        return {"graphics": {"slots": [{"name": name} for name in
                                       ("p0", "p1", "p2", "p3", "p4")]},
                "io_boards": [dict(board) for board in boards]}

    def test_pipe_serials_follow_their_slot(self):
        g = lambda n: 0x51454d554700 | n
        ok = self.pipe_topology(
            {"id": "a", "board_kind": "KONA", "label": "p0", "serial": g(0)},
            {"id": "b", "board_kind": "KONA", "label": "p1", "serial": g(16)})
        model_graphics.check_pipe_serials(ok)
        with self.assertRaisesRegex(ValueError, "another pipe slot"):
            model_graphics.check_pipe_serials(self.pipe_topology(
                {"id": "b", "board_kind": "KONA", "label": "p1",
                 "serial": g(5)}))
        with self.assertRaisesRegex(ValueError, "already presents"):
            model_graphics.check_pipe_serials(self.pipe_topology(
                {"id": "b", "board_kind": "KONA", "label": "p1",
                 "serial": g(16)},
                {"id": "c", "board_kind": "MGRAS", "serial": g(18)}))
        with self.assertRaisesRegex(ValueError, "share serial"):
            model_graphics.check_pipe_serials(self.pipe_topology(
                {"id": "a", "board_kind": "KONA", "label": "p0",
                 "serial": g(0)},
                {"id": "b", "board_kind": "KONA", "label": "p1",
                 "serial": g(0)}))
        with self.assertRaisesRegex(ValueError, "first 4"):
            model_graphics.check_pipe_serials(self.pipe_topology(
                {"id": "e", "board_kind": "KONA", "label": "p4",
                 "serial": g(64)}))

    def test_gbrick_pipes_take_graphics_serials(self):
        serials = {board["label"]: board["serial"] for board in
                   self.topology("onyx3400-ir")["io-boards"]
                   if board["preset"] == "kona"}
        self.assertEqual(serials, {
            "gbrick0-pipe0-pipe": 0x51454d554700,
            "gbrick0-pipe1-pipe": 0x51454d554710,
            "gbrick1-pipe0-pipe": 0x51454d554720,
            "gbrick1-pipe1-pipe": 0x51454d554730})

    def test_slot_name_fits_the_migration_field(self):
        section = self.many_slots("XIO slot {number}")
        section["slots"][0]["names"] = ["g" * 31]
        section["slots"][0]["description"] = "a slot"
        self.resolve(section)
        section["slots"][0]["names"] = ["g" * 32]
        with self.assertRaisesRegex(ResolutionError, "32-byte slot field"):
            self.resolve(section)

    def test_pci_placement(self):
        slots = {slot["name"]: slot for slot in
                 self.topology("origin200-dual")["graphics"]["slots"]}
        self.assertEqual(slots["m0-pci-6"]["placement"],
                         [{"name": "bus", "value": "pci"},
                          {"name": "addr", "value": "6"}])

    def many_slots(self, description):
        return {"max_pipes": 1, "headless": True, "slots": [
            {"names": ["io2", "io3"], "description": description,
             "placement": {"slot": "{name}"},
             "families": [{"family": "mgras", "products": [
                 {"product": "si", "fidelity": "implemented"}]}]}]}

    def test_one_section_names_many_slots(self):
        graphics = self.resolve(self.many_slots("XIO slot {number}"))
        self.assertEqual([slot["name"] for slot in graphics["slots"]],
                         ["io2", "io3"])
        self.assertEqual(graphics["slots"][1]["placement"],
                         [{"name": "slot", "value": "io3"}])
        self.assertEqual([slot["description"] for slot in graphics["slots"]],
                         ["XIO slot 2", "XIO slot 3"])

    def test_slots_need_distinct_descriptions(self):
        with self.assertRaisesRegex(ResolutionError, "describes two slots"):
            self.resolve(self.many_slots("XIO slot"))

    def test_unknown_product_is_refused(self):
        with self.assertRaisesRegex(ResolutionError, "does not list"):
            self.resolve(self.section([{"product": "vpro",
                                        "fidelity": "implemented"}]))

    def test_approximation_needs_its_limitations(self):
        with self.assertRaisesRegex(ResolutionError, "without limitations"):
            self.resolve(self.section([{"product": "si",
                                        "fidelity": "approximated"}]))

    def test_reason_has_no_final_full_stop(self):
        with self.assertRaisesRegex(ResolutionError, "final full stop"):
            self.resolve(self.section(
                [{"product": "si", "fidelity": "implemented"}],
                refusals=[{"family": "kona", "reason": "not here."}]))

    def test_default_must_not_be_refused(self):
        with self.assertRaisesRegex(ResolutionError, "default is refused"):
            self.resolve(self.section(
                [{"product": "si", "fidelity": "implemented"}],
                default={"family": "mgras", "product": "si"},
                refusals=[{"family": "mgras", "product": "si",
                           "reason": "no"}]))

    def test_no_default_means_headless(self):
        section = self.section([{"product": "si", "fidelity": "implemented"}])
        section["headless"] = False
        section["headless_refused"] = "it needs a pipe"
        with self.assertRaisesRegex(ResolutionError, "headless"):
            self.resolve(section)

    def test_product_offered_elsewhere_needs_a_refusal(self):
        here = self.resolve(self.section(
            [{"product": "si", "fidelity": "implemented"}]))
        there = self.resolve(self.section(
            [{"product": "si", "fidelity": "implemented"},
             {"product": "ssi", "fidelity": "implemented"}]))
        with self.assertRaisesRegex(ValueError, "neither offers nor refuses"):
            model_graphics.check_catalogue([{"name": "a", "graphics": here},
                                            {"name": "b", "graphics": there}])
        here = self.resolve(self.section(
            [{"product": "si", "fidelity": "implemented"}],
            refusals=[{"family": "mgras", "product": "ssi",
                       "reason": "two boards do not fit"}]))
        model_graphics.check_catalogue([{"name": "a", "graphics": here},
                                        {"name": "b", "graphics": there}])

    def test_refusal_of_unoffered_product_is_refused(self):
        here = self.resolve(self.section(
            [{"product": "si", "fidelity": "implemented"}],
            refusals=[{"family": "mgras", "product": "ssi",
                       "reason": "two boards do not fit"}]))
        with self.assertRaisesRegex(ValueError, "which no slot offers"):
            model_graphics.check_catalogue([{"name": "a", "graphics": here}])


class StorageTests(unittest.TestCase):
    """Every offering names the writable stores a launcher attaches and the
    images that create them, with no launcher argument templates."""

    @classmethod
    def setUpClass(cls):
        cls.catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sgi-machines.json"])

    def default_processor(self, name):
        topology = next(item for item in self.catalogue["topologies"]
                        if item["name"] == name)
        return topology["nodes"][0]["cpu-slots"][0]["assembly"]["id"]

    def test_every_offering_lists_consistent_storage(self):
        for offering in self.catalogue["offerings"]:
            with self.subTest(options=offering["machine-options"]):
                # processor= appears only where the module is not the
                # topology's own, so the default keeps its old spelling.
                processor = ("" if offering["processor"]["id"] ==
                             self.default_processor(offering["topology"])
                             else f",processor={offering['processor']['id']}")
                if offering.get("launch-recipe") == "ip32":
                    self.assertEqual(offering["machine-options"],
                                     "o2" if offering["topology"] == "o2"
                                     else "o2,variant=o2plus")
                    self.assertEqual(offering["storage"], [])
                    self.assertEqual(offering["firmware"]["kind"], "ip32-prom")
                    self.assertEqual(offering["init-inputs"], [{
                        "name": "boot-prom", "kind": "ip32-prom", "required": True}])
                    continue
                self.assertEqual(
                    offering["machine-options"],
                    f"{offering['product']},topology={offering['topology']}"
                    f",nodes={offering['nodes']}{processor}")
                if offering.get("launch-recipe") == "ip53-discovery":
                    self.assertEqual(offering["firmware"]["kind"], "ip35-prom")
                else:
                    self.assertNotIn("firmware", offering)
                names = [item["name"] for item in offering["storage"]]
                self.assertEqual(len(names), len(set(names)))
                self.assertEqual(
                    [name for name in names if name.endswith("-flash")
                     and name.startswith("node")],
                    [f"node{index}-flash"
                     for index in range(offering["nodes"])])
                for name in names:
                    if name.startswith("nvram") and not name.endswith("-clock"):
                        self.assertIn(f"{name}-clock", names)
                inputs = {item["name"] for item in offering["init-inputs"]}
                for item in offering["storage"]:
                    self.assertFalse(item["read-only"])
                    self.assertTrue(item["initial"] in {"zero", "ip27-nvram",
                                                        "controller-nvram",
                                                        "kona-flash"}
                                    or item["initial"] in inputs)
                for resource in offering["resources"]:
                    self.assertIn(resource["kind"],
                                  {"identity-eeprom", "spd-eeprom", "ds1742",
                                   "boot-flash"})

    def test_two_node_origin2000_deskside(self):
        offering = next(
            item for item in self.catalogue["offerings"]
            if item["machine-options"] ==
            "origin2000,topology=origin2000-deskside,nodes=2")
        self.assertEqual(offering["init-inputs"], [
            {"name": "boot-prom", "kind": "ip27-prom", "required": True},
            {"name": "io-prom", "kind": "io6-prom", "required": True}])
        flash = {"size": 1048576, "read-only": False}
        self.assertEqual(offering["storage"], [
            {"name": "node0-flash", **flash, "initial": "boot-prom"},
            {"name": "node1-flash", **flash, "initial": "boot-prom"},
            {"name": "io0-flash", **flash, "initial": "io-prom"},
            {"name": "nvram0", "size": 32768, "read-only": False,
             "initial": "ip27-nvram"},
            {"name": "nvram0-clock", "size": 16, "read-only": False,
             "initial": "zero"},
            {"name": "module0-nvram", "size": 2048, "read-only": False,
             "initial": "controller-nvram"}])

    def test_ioc3_timekeeper_starts_from_the_fresh_ip27_image(self):
        """An IOC3 Timekeeper with no backing installs the fresh IP27
        image, so a new machine's NVRAM file starts from it too. A
        selected system Timekeeper turns that fixture off. The IP53
        DS1744-compatible profiles start their backed image from zeroes."""
        ds1744_topologies = {
            "origin350", "onyx350-compat", "onyx4-host-serial-compat",
            "tezro-tower-one", "tezro-tower-multi", "tezro-rack",
        }
        for offering in self.catalogue["offerings"]:
            nvram = {item["initial"] for item in offering["storage"]
                     if item["name"].startswith("nvram")
                     and not item["name"].endswith("-clock")}
            expected = (
                {"ip27-nvram"}
                if offering["product"] in {"origin200", "origin2000", "onyx2"}
                else {"zero"}
                if offering["topology"] in ds1744_topologies
                else set()
                if offering.get("launch-recipe") in ("ip53-discovery", "ip32")
                else {"zero"})
            with self.subTest(options=offering["machine-options"]):
                self.assertEqual(nvram, expected)

    def test_origin200_dual_has_a_store_per_board(self):
        offering = next(
            item for item in self.catalogue["offerings"]
            if item["topology"] == "origin200-dual")
        self.assertEqual([item["name"] for item in offering["storage"]],
                         ["node0-flash", "node1-flash", "nvram0",
                          "nvram0-clock", "nvram1", "nvram1-clock",
                          "module0-nvram", "module1-nvram"])

    def test_module_controllers_have_a_configuration_nvram(self):
        """One store per module system controller: each Origin 200 module's
        embedded ELSC, each Origin 2000 or Onyx2 module's MSC, and none
        where an L1 or no controller manages the module."""
        for offering in self.catalogue["offerings"]:
            names = [item["name"] for item in offering["storage"]
                     if item["initial"] == "controller-nvram"]
            expected = {"origin200-dual": 2}.get(
                offering["topology"],
                1 if offering["product"] in {"origin200", "origin2000",
                                             "onyx2"} else 0)
            with self.subTest(options=offering["machine-options"]):
                self.assertEqual(names, [f"module{index}-nvram"
                                         for index in range(expected)])

    def test_scsi_adapters_follow_the_listed_pci_functions(self):
        """Every SCSI host adapter a topology's I/O boards list is one
        offered adapter, whichever spelling the board's own compiler
        record gives its kind: the IO9's ISP12160 counts as the IP45's
        does. Boards built from a preset list no functions here and are
        left to the preset."""
        topologies = {item["name"]: item
                      for item in self.catalogue["topologies"]}
        targets = {"isp1040": list(range(1, 8)),
                   "isp12160": list(range(1, 16))}
        for offering in self.catalogue["offerings"]:
            listed = [function["kind"]
                      for board in topologies[offering["topology"]]
                      ["io-boards"]
                      for function in board.get("pci-functions", [])]
            if not listed:
                continue
            expected = [{"type": "scsi", "bus": f"scsi.{index}",
                         "targets": targets[kind]}
                        for index, kind in enumerate(
                            kind for kind in listed if kind in targets)]
            with self.subTest(options=offering["machine-options"]):
                self.assertEqual(offering["scsi-adapters"], expected)

    def test_ip53_offerings_list_the_io9_isp12160(self):
        for offering in self.catalogue["offerings"]:
            if offering["product"] not in {"origin350", "onyx350", "tezro",
                                           "onyx4-host"}:
                continue
            with self.subTest(options=offering["machine-options"]):
                self.assertEqual(offering["scsi-adapters"], [
                    {"type": "scsi", "bus": "scsi.0",
                     "targets": list(range(1, 16))}])

    def test_networks_follow_the_listed_pci_functions(self):
        """Every onboard Ethernet a topology's I/O boards list is one
        offered network, whichever spelling the board's compiler record
        gives its kind: the IO9's BCM5701 counts as an IOC3 does. Each
        derives its address from the machine's mac= plus its position,
        so none collide, and the first is the default."""
        topologies = {item["name"]: item
                      for item in self.catalogue["topologies"]}
        models = {"ioc3": ("ioc3-ethernet", "sgi-ioc3-eth", "ef"),
                  "bcm5701": ("bcm5701-ethernet", "bcm570x", "tg"),
                  "mace": ("mace-ethernet", "sgi-mace-eth", "ec")}
        for offering in self.catalogue["offerings"]:
            listed = [function["kind"]
                      for board in topologies[offering["topology"]]
                      ["io-boards"]
                      for function in board.get("pci-functions", [])
                      if function["kind"] == "bcm5701"]
            networks = offering["networks"]
            with self.subTest(options=offering["machine-options"]):
                self.assertEqual(
                    [entry["kind"] for entry in networks
                     if entry["kind"] == "bcm5701-ethernet"],
                    ["bcm5701-ethernet"] * len(listed))
                derived = [entry for entry in networks
                           if entry["mac"]["derived"]]
                addresses = [entry["mac"]["default-address"]
                             for entry in derived]
                self.assertEqual(len(set(addresses)), len(addresses))
                self.assertEqual(
                    [entry["mac"]["offset"] for entry in derived],
                    [networks.index(entry) for entry in derived])
                self.assertEqual([entry["default"] for entry in networks],
                                 [True] + [False] * (len(networks) - 1))
                for entry in networks:
                    kind = next(key for key, value in models.items()
                                if value[0] == entry["kind"])
                    self.assertEqual(entry["nic-model"], models[kind][1])
                    self.assertTrue(entry["interface"].startswith(
                        models[kind][2]))
                    self.assertEqual(entry["nic-mac-option"], "macaddr")
                    if entry["mac"]["derived"]:
                        self.assertEqual(entry["mac"]["machine-property"],
                                         "mac")
                        self.assertTrue(entry["mac"]["default-address"]
                                        .startswith("08:00:69:"))
                    else:
                        self.assertEqual(entry["mac"], {"derived": False})
                netboot = offering["netboot-adapter"]
                if netboot != "unsupported":
                    self.assertEqual(netboot, networks[0]["kind"])

    def test_ip53_offerings_list_the_io9_bcm5701(self):
        for offering in self.catalogue["offerings"]:
            if offering["product"] not in {"origin350", "onyx350", "tezro",
                                           "onyx4-host"}:
                continue
            with self.subTest(options=offering["machine-options"]):
                self.assertEqual(offering["networks"], [{
                    "kind": "bcm5701-ethernet",
                    "argument": "-netdev ...,id=net0",
                    "nic-model": "bcm570x", "nic-mac-option": "macaddr",
                    "index": 0, "interface": "tg0", "default": True,
                    "required": False,
                    "mac": {"derived": True,
                            "machine-property": "mac", "offset": 0,
                            "default-address": "08:00:69:12:34:56",
                            "required-prefix": "08:00:69"}}])


class Ip27FirmwareTests(unittest.TestCase):
    """Every SN0 topology carries its product's IP27 flash configuration
    values, and no other generation carries any."""

    def test_sn0_topologies_carry_product_values(self):
        catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sgi-machines.json"])
        expected = {
            "origin200": {"machine-type": 1, "flash-read-cycles": 15,
                          "flash-write-cycles": 4},
            "origin2000": {"machine-type": 0, "flash-read-cycles": 8,
                           "flash-write-cycles": 1},
            "onyx2": {"machine-type": 0, "flash-read-cycles": 8,
                      "flash-write-cycles": 1},
        }
        for topology in catalogue["topologies"]:
            with self.subTest(topology=topology["name"]):
                if topology["generation"] == "sn0":
                    self.assertEqual(topology["ip27-firmware"],
                                     expected[topology["product"]])
                else:
                    self.assertNotIn("ip27-firmware", topology)


class Origin300TimekeeperTests(unittest.TestCase):
    def test_origin300_export_has_per_node_timekeeper_policy(self):
        catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sgi-machines.json"])
        topologies = {row["name"]: row for row in catalogue["topologies"]
                      if row["product"] == "origin300"}
        policy = {"kind": "m48t35-compatible", "year-base": 1968,
                  "initial-ip27-fixture": False}
        for topology in topologies.values():
            fitted = [index for index, board in enumerate(topology["io-boards"])
                      if board["preset"] == "io8" and "ioc3-serial" in board]
            for index, board in enumerate(topology["io-boards"]):
                with self.subTest(topology=topology["name"], board=index):
                    if index in fitted:
                        self.assertEqual(board.get("system-timekeeper"), policy)
                    else:
                        self.assertNotIn("system-timekeeper", board)
            for offering in catalogue["offerings"]:
                if offering["topology"] != topology["name"]:
                    continue
                nvram = [item for item in offering["storage"]
                         if item["name"].startswith("nvram")]
                expected = []
                for index in range(len(fitted)):
                    expected += [
                        {"name": f"nvram{index}", "size": 32768,
                         "read-only": False, "initial": "zero"},
                        {"name": f"nvram{index}-clock", "size": 16,
                         "read-only": False, "initial": "zero"}]
                self.assertEqual(nvram, expected)
        pair = topologies["origin300-two-chassis"]
        self.assertEqual([board["id"] for board in pair["io-boards"]
                          if "system-timekeeper" in board],
                         ["master-xbridge-bridge-f", "slave-xbridge-bridge-f"])

    def test_origin300_timekeeper_rejects_empty_or_unknown_head(self):
        model = load_checked_system_model(
            INPUTS / "origin300.yaml", model_schema=schema(INPUTS))
        policy = {"kind": "m48t35-compatible", "year_base": 1968,
                  "initial_ip27_fixture": False}
        for head in ("xbridge-bridge-e", "absent", "ioc3"):
            altered = copy.deepcopy(model)
            altered["machine"]["emulation"]["system_timekeepers"] = {head: policy}
            with self.subTest(head=head), self.assertRaises(ResolutionError):
                model_composition.validate_system_timekeepers(altered)


class SourcesRequirementTests(unittest.TestCase):
    """Sources are required on hardware claims, not on harness choices."""

    @classmethod
    def setUpClass(cls):
        cls.schema = schema(INPUTS)
        cls.model = load_checked_system_model(
            INPUTS / "octane2.yaml", model_schema=cls.schema)

    def errors(self, model):
        import jsonschema
        return [error.message for error in
                jsonschema.Draft202012Validator(self.schema).iter_errors(model)]

    def test_harness_without_sources_is_accepted(self):
        model = copy.deepcopy(self.model)
        harness = model["machine"]["harness"]
        self.assertIn("sources", harness)
        del harness["provenance"], harness["sources"]
        self.assertEqual(self.errors(model), [])

    def test_hardware_claim_without_sources_is_refused(self):
        model = copy.deepcopy(self.model)
        wiring = model["machine"]["emulation"]["boot_flash_wiring"]
        del wiring["sources"]
        self.assertTrue(any("sources" in message
                            for message in self.errors(model)))
        model = copy.deepcopy(self.model)
        del model["machine"]["emulation"]["boot_flash_wiring"]["provenance"]
        self.assertTrue(any("provenance" in message
                            for message in self.errors(model)))


class FuelTests(unittest.TestCase):
    def test_fuel_offerings_keep_all_console_ports(self):
        """A fresh Fuel flash answers on L1, and IOC3 serial A and B
        follow it in the machine's serial-line order (A on line 1)."""
        catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sgi-machines.json"])
        offerings = [row for row in catalogue["offerings"]
                     if row["product"] == "fuel"]
        self.assertEqual({row["topology"] for row in offerings}, {"fuel"})
        expected = [
            {"kind": "console", "node": 0, "line": 0,
             "argument": "-serial chardev:l1"},
            {"kind": "second-port", "node": 0, "line": 1,
             "argument": "-serial chardev:ioc3_a"},
            {"kind": "second-port", "node": 0, "line": 2,
             "argument": "-serial chardev:ioc3_b"},
        ]
        for row in offerings:
            with self.subTest(topology=row["topology"]):
                self.assertEqual(row["console"], "l1-uart")
                self.assertEqual(row["consoles"], expected)

    def test_bedrock_console_lines_follow_the_machines_line_map(self):
        """Every Origin 300 and Fuel console line is the one the machine
        reads: the L1 first in each node's block of five, then that
        node's IOC3 ports A and B (sgi_sn1_serial_line())."""
        header = (ROOT / "include/hw/mips/sgi/sn-machine.h").read_text()
        per_node = int(re.search(
            r"#define SGI_SN1_SERIAL_LINES_PER_NODE\s+(\d+)", header)[1])
        self.assertIn("node_index * SGI_SN1_SERIAL_LINES_PER_NODE + line",
                      header)
        offsets = {"l1": 0, "ioc3_a": 1, "ioc3_b": 2}
        catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sgi-machines.json"])
        seen = 0
        for row in catalogue["offerings"]:
            if row["product"] not in ("origin300", "fuel"):
                continue
            for console in row["consoles"]:
                chardev = console["argument"].split("chardev:")[1]
                node = int(chardev[len("l1_node"):]) \
                    if chardev.startswith("l1_node") else console["node"]
                offset = 0 if chardev.startswith("l1") else offsets[chardev]
                with self.subTest(topology=row["topology"], chardev=chardev):
                    self.assertEqual(console["node"], node)
                    self.assertEqual(console["line"],
                                     node * per_node + offset)
                seen += 1
        self.assertGreater(seen, 0)
        self.assertTrue(any(c["line"] == per_node for row in
                            catalogue["offerings"]
                            for c in row["consoles"]
                            if c["argument"].endswith("l1_node1")))

    def test_ip35_firmware_names_each_product_machine_type(self):
        """Origin 300, Fuel and the Tezro trial keep reset-mode words; the
        six selected IP53 profiles use type zero and full firmware words."""
        catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sgi-machines.json"])
        ip53_words = {
            "origin350": 0x569cad85,
            "onyx350-compat": 0x569caf85,
            "tezro-tower-one": 0x549bad85,
            "tezro-tower-multi": 0x549bad85,
            "tezro-rack": 0x549bad85,
            "onyx4-host-serial-compat": 0x569dad85,
        }
        for topology in catalogue["topologies"]:
            product = topology["product"]
            if topology["name"] in ("origin3200", "origin3400", "onyx3000",
                                    "onyx3400-ir"):
                # The C-brick's machine-type byte and reset-mode word come
                # from the PROM's CPU-configuration table row.
                self.assertEqual(topology["ip35-firmware"], {
                    "machine-type": 0,
                    "cpu-mode-word": 0x5414a785})
                continue
            if topology["name"] == "origin3900":
                # The Cx-brick reuses the Origin 350's IP53 firmware word.
                self.assertEqual(topology["ip35-firmware"], {
                    "machine-type": 0,
                    "cpu-mode-word": ip53_words["origin350"]})
                continue
            if product not in ("origin300", "fuel", "origin350",
                               "onyx350", "tezro", "onyx4-host"):
                self.assertNotIn("ip35-firmware", topology)
                continue
            with self.subTest(topology=topology["name"]):
                if product in {"origin300", "fuel"}:
                    machine_type = 2 if product == "fuel" else 1
                    expected_mode = 0x549aa985
                elif topology["name"] in ip53_words:
                    machine_type = 0
                    expected_mode = ip53_words[topology["name"]]
                else:
                    machine_type = 1
                    expected_mode = topology["nodes"][0]["cpu-slots"][0][
                        "assembly"]["mode-bits"]
                self.assertEqual(topology["ip35-firmware"], {
                    "machine-type": machine_type,
                    "cpu-mode-word": expected_mode})

    def test_overrides_follow_what_each_machine_binds(self):
        """The Bedrock board values are listed only where a Bedrock, an L1
        and an IO8 or Fuel board bind them; every machine takes mac=, an
        Origin 2000's plain BaseIO takes baseio-preset=, and the processor
        part's PRId is a -cpu property."""
        catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sgi-machines.json"])
        bedrock = ["bedrock-revision", "board-id-word", "ioc3-subsystem-id",
                   "l1-revision", "l1-type-code", "mac"]
        expected = {
            "fuel": (bedrock, ["r14000-prid"]),
            "origin300": (bedrock, ["r14000-prid"]),
            "origin200": (["mac"], ["r10000-prid"]),
            "origin2000": (["baseio-preset", "mac"], ["r10000-prid"]),
            "octane": (["flash-select", "flash-strap", "mac"],
                       ["r10000-prid"]),
        }
        r12000 = ["r12000-prid", "r12000-fpu-id", "r12000-reset-mode",
                  "r12000-scache-bytes", "r12000-scache-block-words"]
        for row in catalogue["offerings"]:
            if row["product"] not in expected:
                continue
            machine, cpu = expected[row["product"]]
            if row["processor"]["part"] == "R12000":
                cpu = r12000
            with self.subTest(offering=row["machine-options"]):
                self.assertEqual(
                    [o["property"] for o in row["overrides"]
                     if o["target"] == "machine"], machine)
                self.assertEqual(
                    [o["property"] for o in row["overrides"]
                     if o["target"] == "cpu"], cpu)

    def test_origin2000_pipe_asks_for_the_baseio_g(self):
        """An Origin 2000's plain BaseIO has no keyboard or mouse port, so
        its graphics record asks a launch with a pipe for the BaseIO-G;
        no other topology's record asks for anything."""
        catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sgi-machines.json"])
        for topology in catalogue["topologies"]:
            graphics = topology.get("graphics") or {}
            with self.subTest(topology=topology["name"]):
                if topology["product"] == "origin2000":
                    self.assertEqual(
                        graphics["machine-properties"]["properties"],
                        [{"name": "baseio-preset", "value": "io6g"}])
                    self.assertFalse(graphics["machine-properties"]
                                     ["reason"].endswith("."))
                else:
                    self.assertNotIn("machine-properties", graphics)

    def test_fuel_export_has_system_timekeeper(self):
        catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sgi-machines.json"])
        topologies = {row["name"]: row for row in catalogue["topologies"]}
        self.assertIn("fuel", topologies)
        self.assertNotIn("fuel-m48t35-compatible", topologies)
        fitted = topologies["fuel"]["io-boards"]
        self.assertEqual([board["preset"] for board in fitted],
                         ["fuel", "odyssey"])
        self.assertEqual(fitted[0]["system-timekeeper"], {
            "kind": "m48t35-compatible", "year-base": 1968,
            "initial-ip27-fixture": False})
        for row in catalogue["offerings"]:
            if row["product"] != "fuel":
                continue
            self.assertEqual(row["default-topology"], "fuel")
            self.assertEqual(row["storage"], [
                {"name": "node0-flash", "size": 16777216, "read-only": False,
                 "initial": "boot-prom"},
                {"name": "nvram0", "size": 32768, "read-only": False,
                 "initial": "zero"},
                {"name": "nvram0-clock", "size": 16, "read-only": False,
                 "initial": "zero"}])
            self.assertEqual(row["init-inputs"], [
                {"name": "boot-prom", "kind": "ip35-prom",
                 "required": True}])

    def test_system_timekeeper_requires_fitted_fuel_board(self):
        model = load_checked_system_model(
            INPUTS / "fuel.yaml", model_schema=schema(INPUTS))
        for field, value in (("generation", "SN0"),):
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



class Origin350ClockPolicyTests(unittest.TestCase):
    def test_ip53_catalogue_selects_backed_ds1744_timekeepers(self):
        catalogue = json.loads(render_qemu_inputs(INPUTS)[
            "hw/mips/sgi/sgi-machines.json"])
        names = {"origin350", "onyx350-compat", "onyx4-host-serial-compat",
                 "tezro-tower-one", "tezro-tower-multi", "tezro-rack"}
        topologies = {row["name"]: row for row in catalogue["topologies"]
                      if row["name"] in names}
        self.assertEqual(set(topologies), names)

        for name, topology in topologies.items():
            with self.subTest(topology=name):
                board = next(row for row in topology["io-boards"]
                             if row["id"] == "io9")
                self.assertEqual(board["system-timekeeper"], {
                    "kind": "ds1744", "year-base": 1968,
                    "initial-ip27-fixture": False})
                self.assertNotIn("ds1742", board)
                self.assertNotIn("ds1742-fresh-year-base", board)
                offerings = [row for row in catalogue["offerings"]
                             if row["topology"] == name]
                self.assertTrue(offerings)
                for offering in offerings:
                    storage = {item["name"]: item
                               for item in offering["storage"]}
                    self.assertEqual(storage["nvram0"]["size"], 32768)
                    self.assertEqual(storage["nvram0-clock"]["size"], 16)
                    self.assertEqual(storage["nvram0"]["initial"], "zero")
                    self.assertEqual(storage["nvram0-clock"]["initial"],
                                     "zero")
                    self.assertFalse(any(
                        resource["kind"] == "ds1742"
                        for resource in offering["resources"]))


if __name__ == "__main__":
    unittest.main()
