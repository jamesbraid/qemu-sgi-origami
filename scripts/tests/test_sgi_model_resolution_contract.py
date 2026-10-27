# SPDX-License-Identifier: BSD-3-Clause

#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyyaml>=6"]
# ///
"""Focused tests for compositional system-model resolution."""

import copy
import json
import pathlib
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
INPUTS = ROOT / "hw/mips/sgi/models"
FIXTURES = pathlib.Path(__file__).with_name("system-model-fixtures")
sys.path.insert(0, str(ROOT / "scripts"))

from sgi_model import system_model as _resolver
from sgi_model.model_validation import load_checked_system_model, schema


def load_resolver():
    return _resolver


class SystemModelResolutionTests(unittest.TestCase):
    def test_component_ids_require_stable_strings(self):
        for component in ({}, {"id": None}, {"id": 1}, {"id": []}):
            with self.subTest(component=component), self.assertRaisesRegex(
                    self.resolver.ResolutionError, "no stable identifier"):
                self.resolver.model_composition.validate_component_ids(
                    {"components": [component]})

    def test_unknown_composition_fields_are_rejected_even_when_null(self):
        for field in ("implementation_projection", "config_graph_instances", "layer"):
            for value in (None, {}, {"address_spaces": []}):
                with self.subTest(field=field, value=value), self.assertRaisesRegex(
                        self.resolver.ResolutionError, f"unsupported field '{field}'"):
                    self.load_layer_changes([
                        ("origin200.yaml", lambda document: document.update({field: value}))])

    # Several tests load the same model inputs directly instead of
    # using load_layer_changes's mutated copy:
    # resolving each is 100-200ms, and dozens of tests read one unchanged,
    # so setUpClass resolves each exactly once and tests take a fresh deep
    # copy from real_model() instead of paying for their own resolution.
    REAL_COMPOSITIONS = (
        "origin200", "origin200-dual", "origin200-gigachannel",
        "origin2000", "origin2000-rack", "origin300",
    )

    @classmethod
    def setUpClass(cls):
        cls.resolver = load_resolver()
        cls._real_models = {
            name: cls.resolver.load_system_model(
                INPUTS / f"{name}.yaml")
            for name in cls.REAL_COMPOSITIONS
        }
        # load_layer_changes's own copytree source: staged once here instead
        # of copying the model input tree on every one of its 16 rounds.
        cls._staged_machines = tempfile.mkdtemp(prefix="origami-machines-")
        shutil.copytree(INPUTS, cls._staged_machines,
                        dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("*.json"))

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls._staged_machines, ignore_errors=True)

    def real_model(self, name):
        """A fresh deep copy of `name`'s once-per-class-resolved composition."""
        return copy.deepcopy(self._real_models[name])

    def reload_model(self, model):
        return self.resolver._validate_resolved_system_model(copy.deepcopy(model))

    def test_dual_loads_each_module_instance_exactly_once(self):
        # Counts real _read_yaml calls during one fresh resolution;
        # real_model()'s class-cached copy would read zero.
        path = INPUTS / "origin200-dual.yaml"
        with patch.object(self.resolver, "_read_yaml", wraps=self.resolver._read_yaml) as read:
            self.resolver.load_system_model(path)
        self.assertEqual(sum(call.args[0].name == "logic-carrier.yaml"
                             and call.args[1] == "module" for call in read.call_args_list), 2)

    def test_cpu_selection_attaches_the_catalogued_default_to_its_slots(self):
        model = self.real_model("origin200")
        cpu = next(item for item in model["components"] if item["id"] == "cpu0")
        self.assertEqual(
            cpu["identity"],
            {
                "processor_identification": "0x0926",
                "fpu_identification": "0x0900",
                "cache_clock_mhz": 120,
                "clock_mhz": 180,
                "cache_mib": 1,
                "variants": [
                    {"clock_mhz": 180, "cache_mib": 1},
                    {"clock_mhz": 195, "cache_mib": 4},
                    {"clock_mhz": 180, "cache_mib": 2},
                    {"clock_mhz": 225, "cache_mib": 2},
                ],
            },
        )

    def test_cpu_selection_offered_lists_the_selectable_modules(self):
        model = self.real_model("origin200")
        self.assertEqual(
            [(option["part"], option["clock_mhz"], option["cache_mib"],
              option["processors_per_node"])
             for option in model["machine"]["processor_options"]],
            [("r10000", 180, 1, [1, 2]), ("r10000", 180, 2, [1, 2]),
             ("r10000", 225, 2, [2]), ("r12000", 270, 4, [2])],
        )

    def test_cpu_selection_offered_rejects_an_uncatalogued_module(self):
        def edit(document):
            document["layer"]["cpu_selection"]["offered"].append(
                {"part": "r12000", "clock_mhz": 300, "cache_mib": 8,
                 "processors_per_node": [2]})

        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                "r12000 300 MHz/8 MiB, which is not a catalogued variant"):
            self.load_layer_changes([
                ("layers/products/origin200.yaml", edit),
            ])

    def test_cpu_selection_offered_must_start_with_the_default(self):
        def edit(document):
            offered = document["layer"]["cpu_selection"]["offered"]
            offered.append(offered.pop(0))

        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                "list the default processor first"):
            self.load_layer_changes([
                ("layers/products/origin200.yaml", edit),
            ])

    def test_cpu_selection_fills_identity_registers_from_the_catalogue(self):
        # origin2000-rack's CPU components author no identity at all
        # (unlike Origin 200's, which authors it directly); the catalogue
        # supplies processor_identification, fpu_identification and
        # cache_clock_mhz the same way it already supplies clock_mhz and
        # cache_mib.
        model = self.real_model("origin2000-rack")
        cpu = next(item for item in model["components"] if item["id"] == "node-board-1-cpu-a")
        self.assertEqual(cpu["identity"]["processor_identification"], "0x0926")
        self.assertEqual(cpu["identity"]["fpu_identification"], "0x0900")
        self.assertEqual(cpu["identity"]["cache_clock_mhz"], 120)

    def test_cpu_selection_rejects_a_directly_authored_identity_register_disagreement(self):
        def contradict_prid(document):
            for component in document["layer"]["data"]["components"]:
                if component.get("id") == "cpu0":
                    component["identity"]["processor_identification"] = "0x0999"

        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                "authors processor_identification directly with a value "
                "disagreeing with the catalogued part"):
            self.load_layer_changes([
                ("layers/modules/logic-carrier.yaml", contradict_prid),
            ])

    def test_processor_reset_mode_is_attached_from_the_catalogue(self):
        model = self.real_model("origin200")
        architecture = self.resolver._layer(INPUTS / "layers/architectures/sn0.yaml")
        catalogued = next(
            variant["reset_mode"]
            for entry in architecture["data"]["processors"]
            for variant in entry["variants"]
            if variant.get("clock_mhz") == 180 and variant.get("cache_mib") == 1
        )
        self.assertEqual(
            model["machine"]["firmware_config"]["processor_reset_mode"],
            catalogued,
        )

    def test_processor_reset_mode_rejects_disagreement_with_the_catalogue(self):
        def contradict_reset_mode(document):
            document["layer"]["data"]["machine"]["firmware_config"][
                "processor_reset_mode"
            ] = dict(
                self.real_model("origin200")["machine"]["firmware_config"]["processor_reset_mode"],
                cache_test_mode=True)

        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                "processor_reset_mode disagrees with the catalogued reset mode"):
            self.load_layer_changes([
                ("layers/products/origin200.yaml", contradict_reset_mode),
            ])

    def test_cpu_selection_rejects_an_uncatalogued_part(self):
        def rename_part(document):
            for component in document["layer"]["data"]["components"]:
                if component.get("id") == "cpu0":
                    component["hardware_kind"] = "r99999"

        with self.assertRaisesRegex(
                self.resolver.ResolutionError, "uncatalogued part 'r99999'"):
            self.load_layer_changes([
                ("layers/modules/logic-carrier.yaml", rename_part),
            ])

    def test_cpu_selection_rejects_a_component_authoring_a_catalogued_field(self):
        def add_clock(document):
            for component in document["layer"]["data"]["components"]:
                if component.get("id") == "cpu0":
                    component["identity"]["clock_mhz"] = 180

        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                "authors clock_mhz directly"):
            self.load_layer_changes([
                ("layers/modules/logic-carrier.yaml", add_clock),
            ])

    def test_cpu_selection_rejects_an_unlisted_default_variant(self):
        def unlisted_default(document):
            document["layer"]["cpu_selection"]["default"] = {
                "clock_mhz": 180, "cache_mib": 4,
            }

        with self.assertRaisesRegex(
                self.resolver.ResolutionError, "is not a catalogued 'r10000' variant"):
            self.load_layer_changes([
                ("layers/products/origin200.yaml", unlisted_default),
            ])

    def test_memory_kits_derive_the_population_fields_they_replace(self):
        model = self.real_model("origin200")
        population = model["composition"]["population"]
        self.assertEqual(population["dimms_per_node"], [2])
        self.assertEqual(population["memory_mib_per_node"], [64])
        self.assertEqual(population["memory_bank_mib_per_node"], [[64, 0, 0, 0]])

    def test_memory_kits_rejects_an_unknown_kit(self):
        def unknown_kit(document):
            document["layer"]["memory_kits_per_node"] = ["kit-does-not-exist"]

        with self.assertRaisesRegex(
                self.resolver.ResolutionError, "unknown memory kit 'kit-does-not-exist'"):
            self.load_layer_changes([
                ("layers/products/origin200.yaml", unknown_kit),
            ])

    def test_memory_kits_rejects_a_directly_authored_field(self):
        def add_memory_mib(document):
            document["layer"]["population"]["memory_mib_per_node"] = [64]

        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                "authors memory_mib_per_node directly"):
            self.load_layer_changes([
                ("layers/products/origin200.yaml", add_memory_mib),
            ])

    def test_memory_kits_per_node_must_match_node_count(self):
        def extra_kit(document):
            document["layer"]["memory_kits_per_node"] = ["kit-64", "kit-64"]

        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                "memory_kits_per_node has 2 entries for 1 nodes"):
            self.load_layer_changes([
                ("layers/products/origin200.yaml", extra_kit),
            ])

    def test_memory_kits_place_banks_at_their_named_bank_positions(self):
        model = self.real_model("origin300")
        population = model["composition"]["population"]
        self.assertEqual(population["dimms_per_node"], [2])
        self.assertEqual(population["memory_mib_per_node"], [512])
        self.assertEqual(
            population["memory_bank_mib_per_node"],
            [[0, 0, 256, 256, 0, 0, 0, 0]])

    def test_memory_kits_rejects_bank_positions_with_the_wrong_count(self):
        def short_positions(document):
            for kit in document["layer"]["memory_kits"]:
                kit["bank_positions"] = [2]

        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                "bank_positions must name 2 distinct locational banks below 8"):
            self.load_layer_changes([
                ("layers/assemblies/ip45.yaml", short_positions),
            ], name="origin300")

    def test_memory_kits_rejects_bank_positions_out_of_range(self):
        def out_of_range(document):
            for kit in document["layer"]["memory_kits"]:
                kit["bank_positions"] = [2, 8]

        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                "bank_positions must name 2 distinct locational banks below 8"):
            self.load_layer_changes([
                ("layers/assemblies/ip45.yaml", out_of_range),
            ], name="origin300")

    def test_memory_kits_rejects_repeated_bank_positions(self):
        def repeated_positions(document):
            for kit in document["layer"]["memory_kits"]:
                kit["bank_positions"] = [2, 2]

        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                "bank_positions must name 2 distinct locational banks below 8"):
            self.load_layer_changes([
                ("layers/assemblies/ip45.yaml", repeated_positions),
            ], name="origin300")

    def test_origin200_memory_variant_flows_into_compiled_descriptor(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        from sgi_model.model_compile import compile_model

        def select_variant(document):
            document["layer"]["memory_kits_per_node"] = ["kit-128"]

        model = self.load_layer_changes([
            ("layers/products/origin200.yaml", select_variant),
        ])
        node = compile_model(model)["nodes"][0]
        self.assertEqual(sum(node["bank_mib"]), 128)

    def load_layer_changes(self, changes, name="origin200"):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            shutil.copytree(self._staged_machines, root, dirs_exist_ok=True)
            for relative, mutate in changes:
                path = root / relative
                document = yaml.safe_load(path.read_text())
                mutate(document)
                path.write_text(yaml.safe_dump(document, sort_keys=False))
            return self.resolver.load_system_model(root / f"{name}.yaml")

    def load_module(self, contract, validate=True, **instance):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = self.address_composition(root)
            source = self.module_composition(root / "module", contract)
            composition = yaml.safe_load(path.read_text())
            composition["module_instances"] = [{
                "model": source.relative_to(root).as_posix(), "prefix": "node0-",
                "parent": "hub", **instance,
            }]
            path.write_text(yaml.safe_dump(composition))
            if not validate:
                return self.resolver.resolve_system_model(path, composition)
            return self.resolver.load_system_model(path)

    def test_module_layer_imports_physical_inventory_without_a_product(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.address_composition(directory)
            module = path.with_name("module.yaml")
            module.write_text(yaml.safe_dump({"layer_version": 1, "layer": {
                "kind": "module", "id": "module", "requires": {"assembly": "ip29"},
                "data": {"components": [{"id": "chassis", "kind": "chassis"}]},
            }}))
            document = yaml.safe_load(path.read_text())
            document["module_instances"] = [{"model": "module.yaml", "prefix": "node0-", "parent": "hub"}]
            path.write_text(yaml.safe_dump(document))
            model = self.resolver.load_system_model(path)
            self.assertEqual(model["components"][-1], {
                "id": "node0-chassis", "kind": "chassis", "regions": [], "parent": "hub"})
            source = yaml.safe_load(module.read_text())
            for mutate, message in (
                    (lambda layer: layer["requires"].update(assembly="other"), "requires assembly 'other'"),
                    (lambda layer: layer["data"].update(machine={}), "only physical graph collections"),
                    (lambda layer: layer.update(kind="product"), "not a module layer")):
                document = copy.deepcopy(source)
                mutate(document["layer"])
                module.write_text(yaml.safe_dump(document))
                with self.assertRaisesRegex(self.resolver.ResolutionError, message):
                    self.resolver.load_system_model(path)

    def test_module_instance_places_its_root_in_a_physical_slot(self):
        # A board fitted into a connector is a product-level module
        # instance; the composition names the slot it occupies, as a
        # nested module instance does.
        with tempfile.TemporaryDirectory() as directory:
            path = self.address_composition(directory)
            module = path.with_name("board.yaml")
            layer = {"layer_version": 1, "layer": {
                "kind": "module", "id": "board",
                "data": {"components": [{"id": "board", "name": "Board",
                                         "kind": "board"}]},
            }}
            module.write_text(yaml.safe_dump(layer))
            source = yaml.safe_load(path.read_text())
            instance = {"model": "board.yaml", "prefix": "io2-", "parent": "hub",
                        "physical_slot": {"kind": "io", "number": 2}}
            document = copy.deepcopy(source)
            document["module_instances"] = [instance]
            path.write_text(yaml.safe_dump(document))
            # Resolution alone: this fixture has no module component for
            # the slot's own uniqueness check to scope it to.
            model = self.resolver.resolve_system_model(path, document)
            root = next(item for item in model["components"]
                        if item["id"] == "io2-board")
            self.assertEqual(root["physical_slot"], {"kind": "io", "number": 2})

            for name, change, message in (
                    ("not a mapping", {"physical_slot": 2}, "physical_slot mapping"),
                    ("root authors its own", None, "already authors one")):
                with self.subTest(name=name):
                    if change is None:
                        authored = copy.deepcopy(layer)
                        authored["layer"]["data"]["components"][0]["physical_slot"] = {
                            "kind": "io", "number": 3}
                        module.write_text(yaml.safe_dump(authored))
                        document["module_instances"] = [instance]
                    else:
                        module.write_text(yaml.safe_dump(layer))
                        document["module_instances"] = [{**instance, **change}]
                    path.write_text(yaml.safe_dump(document))
                    with self.assertRaisesRegex(self.resolver.ResolutionError, message):
                        self.resolver.resolve_system_model(path, document)

    BOARD_ON_AN_UPSTREAM_BUS = {
        "components": [
            {"id": "board", "name": "Board", "kind": "board"},
            {"id": "bridge", "name": "Bridge", "kind": "pci-host-bridge",
             "parent": "board"},
        ],
        "buses": [{"id": "pci", "name": "PCI", "kind": "pci", "master": "bridge"}],
        "connections": [
            {"component": "bridge", "bus": "upstream", "role": "bridge-upstream"},
            {"component": "bridge", "bus": "pci", "role": "bridge-downstream"},
        ],
    }

    def test_module_instance_binds_its_upstream_bus_to_an_enclosing_one(self):
        # A fitted board names the bus it plugs into by a placeholder its
        # layer does not declare; each instance binds it to a bus of the
        # enclosing graph, so one board layer fits any slot. The instance's
        # own buses are still prefixed.
        graph = self.load_module(self.BOARD_ON_AN_UPSTREAM_BUS, validate=False,
                                 bus_bindings={"upstream": "xio"})
        connections = [item for item in graph["connections"]
                       if item["component"] == "node0-bridge"]
        self.assertEqual([(item["bus"], item["role"]) for item in connections], [
            ("xio", "bridge-upstream"),
            ("node0-pci", "bridge-downstream"),
        ])

    def test_module_instance_bus_bindings_are_checked(self):
        for name, bindings, message in (
                ("an unbound placeholder", None, "undeclared bus 'upstream'"),
                ("a binding for a bus the layer declares", {"upstream": "xio", "pci": "y"},
                 "binds bus 'pci'"),
                ("a binding nothing uses", {"upstream": "xio", "spare": "y"},
                 "binds bus 'spare'"),
                ("a binding that is not a mapping", ["upstream"], "bus_bindings"),
                ("a binding to a bus the enclosing graph lacks",
                 {"upstream": "no-such-bus"}, "binds 'upstream' to unknown bus 'no-such-bus'"),
        ):
            with self.subTest(name=name):
                extra = {} if bindings is None else {"bus_bindings": bindings}
                with self.assertRaisesRegex(self.resolver.ResolutionError, message):
                    self.load_module(self.BOARD_ON_AN_UPSTREAM_BUS, validate=False,
                                     **extra)

    def test_module_instance_namespaces_optional_link_endpoints(self):
        contract = {
            "components": [
                {"id": "io9", "kind": "board", "hardware_kind": "io9"},
                {"id": "rtc", "kind": "rtc", "hardware_kind": "ds1742",
                 "parent": "io9"},
                {"id": "ioc4", "kind": "io-controller", "hardware_kind": "ioc4",
                 "parent": "io9"},
            ],
            "links": [{"kind": "ioc4-clock", "source": "rtc", "target": "ioc4"}],
        }
        model = self.load_module(contract, validate=False)

        self.assertEqual(model["links"], [{
            "kind": "ioc4-clock", "source": "node0-rtc", "target": "node0-ioc4",
        }])

    def test_module_instance_omission_drops_a_link_to_the_omitted_component(self):
        for omitted in ("rtc", "ioc4"):
            with self.subTest(omitted=omitted):
                contract = {
                    "components": [
                        {"id": "io9", "kind": "board", "hardware_kind": "io9"},
                        {"id": "rtc", "kind": "rtc", "hardware_kind": "ds1742",
                         "parent": "io9"},
                        {"id": "ioc4", "kind": "io-controller",
                         "hardware_kind": "ioc4", "parent": "io9"},
                    ],
                    "links": [{"kind": "ioc4-clock", "source": "rtc",
                               "target": "ioc4"}],
                }
                next(component for component in contract["components"]
                     if component["id"] == omitted)["optional"] = {"node": 1}

                model = self.load_module(contract, validate=False)

                self.assertNotIn(f"node0-{omitted}", {
                    item["id"] for item in model["components"]
                })
                self.assertEqual(model["links"], [])

    def test_legacy_module_without_links_keeps_the_field_absent(self):
        model = self.load_module({
            "components": [{"id": "board", "kind": "board"}],
        }, validate=False)

        self.assertNotIn("links", model)

    def test_nested_module_composition_namespaces_link_endpoints(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            leaf = root / "rtc.yaml"
            leaf.write_text(yaml.safe_dump({
                "layer_version": 1,
                "layer": {
                    "kind": "module", "id": "rtc-module",
                    "requires": {"assembly": "ip29"},
                    "data": {
                        "components": [
                            {"id": "io9", "kind": "board", "hardware_kind": "io9"},
                            {"id": "rtc", "kind": "rtc", "hardware_kind": "ds1742",
                             "parent": "io9"},
                            {"id": "ioc4", "kind": "io-controller",
                             "hardware_kind": "ioc4", "parent": "io9"},
                        ],
                        "links": [{"kind": "ioc4-clock", "source": "rtc",
                                   "target": "ioc4"}],
                    },
                },
            }))
            parent = root / "parent.yaml"
            parent.write_text(yaml.safe_dump({
                "layer_version": 1,
                "layer": {
                    "kind": "module", "id": "parent-module",
                    "requires": {"assembly": "ip29"},
                    "data": {
                        "components": [
                            {"id": "chassis", "kind": "chassis"},
                        ],
                        "module_instances": [{
                            "model": "rtc.yaml", "id": "io9-slot",
                            "parent": "chassis",
                            "physical_slot": {"kind": "io", "number": 9},
                        }],
                    },
                },
            }))

            layer = self.resolver._layer(parent)

        self.assertEqual(layer["data"]["links"], [{
            "kind": "ioc4-clock", "source": "io9-slot-rtc",
            "target": "io9-slot-ioc4",
        }])

    def module_composition(self, directory, contract):
        root = pathlib.Path(directory)
        root.mkdir()
        data = {
            field: copy.deepcopy(contract.get(field, []))
            for field in self.resolver.MODULE_GRAPH_FIELDS
        }
        if "links" in contract:
            data["links"] = copy.deepcopy(contract["links"])
        path = root / "module.yaml"
        path.write_text(yaml.safe_dump({
            "layer_version": 1,
            "layer": {
                "kind": "module",
                "id": "fixture-module",
                "requires": {"assembly": "ip29"},
                "data": data,
            },
        }, sort_keys=False))
        return path

    def test_origin300_layering_preserves_existing_model_content(self):
        model = self.real_model("origin300")
        self.assertEqual(model.pop("evidence_policy"), "strict")
        composition = model.pop("composition")
        model["population"] = composition.pop("population")
        # The base module leaves both external cable ports to the composition.
        self.assertEqual(
            [(item["component"], item["occupancy"])
             for item in composition.pop("connector_population")],
            [("xio-connector", "empty"), ("link-connector", "empty")])
        self.assertEqual(composition, {
            "architecture": "sn1",
            "platform": "ip35",
            "assembly": "ip45",
            "product": "origin300",
            "layers": [
                {
                    "kind": "architecture",
                    "id": "sn1"
                },
                {
                    "kind": "assembly",
                    "id": "ip45"
                },
                {
                    "kind": "module",
                    "id": "origin300-base-module"
                },
                {
                    "kind": "product",
                    "id": "origin300"
                }
            ],
            "topology": {
                "min_nodes": 1,
                "max_nodes": 1,
                "supported_cpu_populations": [
                    [
                        2
                    ],
                    [
                        4
                    ]
                ],
                "provenance": "derived",
                "sources": [
                    "qemu:docs/system/mips/sgi/sn.rst#sgi-sn-topology",
                    "007-4365-002 printed pages 1 and 10 to 12: two or four processors and 512 MB to 4 GB",
                    "108-0263-004 printed page 2-18: two- and four-processor 500 MHz R14000 configurations with 2 MB cache"
                ]
            },
            "limits": {
                "nodes": 1,
                "cpus_per_node": 4,
                "dimms_per_node": 4,
                "provenance": "vendor-document",
                "sources": [
                    "007-4365-002 printed pages 1 and 10 to 12: two or four processors and 512 MB to 4 GB",
                    "007-4365-002 printed pages 10 to 12, Figure 1-5"
                ],
                "memory_banks_per_node": 8,
                "supported_bank_sizes_mib": [256]
            }
        })
        self.assertNotIn("nodes", model["machine"])
        self.assertEqual(model["machine"]["name"], "Origin 300")
        self.assertEqual(model["machine"]["board_ip"], "IP45")
        self.assertEqual(model["population"]["nodes"], 1)
        self.assertEqual(model["population"]["cpus_per_node"], [2])
        self.assertEqual(model["population"]["dimms_per_node"], [2])
        self.assertEqual(model["population"]["memory_mib_per_node"], [512])
        self.assertEqual(
            model["population"]["memory_bank_mib_per_node"],
            [[0, 0, 256, 256, 0, 0, 0, 0]],
        )
        component_ids = {component["id"] for component in model["components"]}
        self.assertTrue(
            {
                "cpu0", "cpu1", "cpu2", "cpu3", "node-memory", "bedrock",
                "xbridge", "xbridge-crossbar", "xbridge-bridge-e",
                "xbridge-bridge-f", "ioc3", "scsi", "usb",
                "serial-id-eeprom", "l1",
            } <= component_ids
        )
        cpus_by_id = {
            component["id"]: component for component in model["components"]
            if component.get("kind") == "cpu"
        }
        self.assertEqual(
            {cid: cpu["processor_slice"] for cid, cpu in cpus_by_id.items()},
            {"cpu0": "A", "cpu2": "B", "cpu1": "C", "cpu3": "D"},
        )
        firmware = model["machine"]["emulation"]["firmware_payload"]
        self.assertEqual(firmware["size"], "0x1686a8")
        self.assertEqual(
            [view["address"] for view in firmware["views"]],
            [
                "0x000_001fc00000", "0x100_18000000",
                "0x100_1fc00000", "0x100_38000000",
            ],
        )

    def shared_i2c_model(self):
        return {
            "components": [{"id": name} for name in ("adapter", "msc", "nvram")],
            "buses": [{"id": "management", "kind": "i2c",
                       "masters": ["adapter", "msc"]}],
            "connections": [
                {"component": "adapter", "bus": "management", "role": "master"},
                {"component": "msc", "bus": "management", "role": "master",
                 "i2c_address": 8},
                {"component": "nvram", "bus": "management", "role": "endpoint",
                 "i2c_address": 0x50, "i2c_address_count": 8},
            ],
        }

    def test_shared_i2c_module_prefix_and_omission(self):
        model = self.shared_i2c_model()
        model["components"].insert(0, {"id": "module", "kind": "module"})
        for component in model["components"][1:]:
            component["parent"] = "module"
        def expand(omit=()):
            return self.load_module(
                model, prefix="m1-",
                omit_buses=list(omit))
        graph = expand()
        self.assertEqual(graph["buses"][1]["masters"], ["m1-adapter", "m1-msc"])
        model["components"][1]["optional"] = {"node": 1}
        with self.assertRaisesRegex(self.resolver.ResolutionError, "omits master"):
            expand()
        self.assertEqual([bus["id"] for bus in expand(["management"])["buses"]], ["xio"])

    def test_shared_i2c_addressed_master(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.address_composition(directory, product_data=self.shared_i2c_model())
            model = self.resolver.load_system_model(path)
        self.assertEqual(model["connections"][1]["i2c_address"], 8)

    def test_shared_i2c_invalid_attachments(self):
        cases = (
            (lambda m: m["buses"][0].update(master="adapter"), "both"),
            (lambda m: m["buses"][0].update(masters=[]), "nonempty"),
            (lambda m: m["buses"][0].update(masters=["adapter", "adapter"]), "unique"),
            (lambda m: m["buses"][0].update(masters=["adapter", "missing"]), "unknown"),
            (lambda m: m["connections"].pop(0), "requires one connection"),
            (lambda m: m["buses"][0].update(masters=["adapter"]), "undeclared"),
            (lambda m: m["connections"].append(dict(m["connections"][1])), "duplicate"),
            (lambda m: m["connections"][2].update(role="bridge-upstream"), "role"),
            (lambda m: m["connections"][0].update(i2c_address_count=2), "without"),
            (lambda m: m["connections"][1].update(i2c_address=0x50), "overlaps"),
            (lambda m: m["buses"][0].update(kind="pci"), "only on I2C"),
        )
        for mutate, message in cases:
            with self.subTest(message=message):
                model = self.shared_i2c_model()
                mutate(model)
                with tempfile.TemporaryDirectory() as directory:
                    path = self.address_composition(directory, product_data=model)
                    with self.assertRaisesRegex(self.resolver.ResolutionError, message):
                        self.resolver.load_system_model(path)

    def address_composition(self, directory, *, role="reference",
                            architecture_emulation=False, product_emulation=False,
                            capacity_metadata=None, product_data=None):
        root = pathlib.Path(directory)
        address_space = {
            "id": "xio",
            "name": "XIO I/O",
            "selector": 1,
            "unmapped_base": "0x9200_0000_0000_0000",
            "node_stride": "0x1_0000_0000",
        }
        if architecture_emulation:
            address_space["emulation"] = {"carrier_base": "0x200_0000_0000"}

        machine = {
            "name": "Address projection fixture",
            "generation": "SN0",
            "board_ip": "IP29",
            "architecture_ip": "IP27",
            "kernel_ip": "IP27",
        }
        if role is not None:
            machine["model_role"] = role
        if role == "emulation-target":
            machine["emulation"] = {
                "machine_name": "address-projection-fixture",
                "display_description": "Address projection fixture",
                "profile": "base",
            }
        component = {
            "id": "hub",
            "name": "Hub",
            "kind": "node-asic",
            "provenance": "derived",
        }
        if product_emulation:
            component["emulation"] = {"status": "implemented"}

        documents = {
            "architecture.yaml": {
                "layer_version": 1,
                "layer": {
                    "kind": "architecture",
                    "id": "sn0",
                    "data": {
                        "model_version": 1,
                        "space_selection": {
                            "mechanism": "processor-attribute",
                            "bits": "58:57",
                            "count": 4,
                        },
                        "address_spaces": [address_space],
                    },
                },
            },
            "assembly.yaml": {
                "layer_version": 1,
                "layer": {
                    "kind": "assembly",
                    "id": "ip29",
                    "platform": "ip27",
                    "requires": {"architecture": "sn0"},
                    "capacity": {
                        "cpu_slots_per_node": 2,
                        "dimm_slots_per_node": 8,
                    },
                },
            },
            "product.yaml": {
                "layer_version": 1,
                "layer": {
                    "kind": "product",
                    "id": "address-product",
                    "requires": {"assembly": "ip29"},
                    "topology": {
                        "min_nodes": 1,
                        "max_nodes": 1,
                        "supported_cpu_populations": [[1], [2]],
                    },
                    "population": {
                        "nodes": 1,
                        "cpus_per_node": [1],
                        "dimms_per_node": [0],
                        "memory_mib_per_node": [64],
                    },
                    "data": {
                        "machine": machine,
                        "components": [component],
                        "buses": [{
                            "id": "xio",
                            "name": "XIO",
                            "kind": "xio",
                            "master": "hub",
                        }],
                        "connections": [{
                            "component": "hub",
                            "bus": "xio",
                            "role": "master",
                        }],
                    },
                },
            },
        }
        if product_data:
            documents["product.yaml"]["layer"]["data"].update(product_data)
        if capacity_metadata:
            documents["assembly.yaml"]["layer"]["capacity"].update(
                capacity_metadata
            )
        for name, document in documents.items():
            (root / name).write_text(yaml.safe_dump(document, sort_keys=False))

        composition = {
            "configuration_version": 1,
            "layers": [
                "architecture.yaml",
                "assembly.yaml",
                "product.yaml",
            ],
        }
        path = root / "model.yaml"
        path.write_text(yaml.safe_dump(composition, sort_keys=False))
        return path

    def assert_no_emulation(self, value):
        if isinstance(value, dict):
            self.assertNotIn("emulation", value)
            for child in value.values():
                self.assert_no_emulation(child)
        elif isinstance(value, list):
            for child in value:
                self.assert_no_emulation(child)

    def test_layered_fixtures_are_rejected(self):
        cases = (
            ("missing layer", "missing-layer.yaml",
             r"missing layer .*does-not-exist\.yaml"),
            ("incompatible layer requirement", "incompatible.yaml",
             r"assembly 'ip45' requires architecture 'sn1', got 'sn0'"),
            ("more CPUs than the assembly has slots", "too-many-cpus.yaml",
             r"node 0 populates 3 CPUs, but assembly 'ip29' has 2 slots"),
            ("more DIMMs than the assembly has slots", "too-many-dimms.yaml",
             r"node 0 populates 9 DIMMs, but assembly 'ip29' has 8 slots"),
            ("more nodes than the topology permits", "too-many-nodes.yaml",
             r"product 'origin200' permits at most 2 nodes, got 3"),
        )
        for name, fixture, message in cases:
            with self.subTest(name=name):
                with self.assertRaisesRegex(
                        self.resolver.ResolutionError, message):
                    self.resolver.load_system_model(FIXTURES / fixture)

    def test_assembly_graph_conflict_names_both_owners(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.address_composition(directory)
            assembly_path = pathlib.Path(directory) / "assembly.yaml"
            assembly = yaml.safe_load(assembly_path.read_text())
            assembly["layer"]["data"] = {"components": [{"id": "other"}]}
            assembly_path.write_text(yaml.safe_dump(assembly))
            with self.assertRaisesRegex(
                    self.resolver.ResolutionError,
                    r"conflicting resolved field 'components'.*assembly 'ip29'.*product 'address-product'"):
                self.resolver.load_system_model(path)

    def test_layers_remain_unique_and_required(self):
        path = INPUTS / "origin200.yaml"
        document = yaml.safe_load(path.read_text())
        for index, kind in enumerate(("architecture", "assembly", "module", "product")):
            layers = document["layers"]
            for references, message in (
                    (layers + [layers[index]], f"as its {kind}"),
                    (layers[:index] + layers[index + 1:],
                     "missing module 'logic-carrier'" if kind == "module" else f"missing {kind} layers")):
                with self.subTest(kind=kind, references=references):
                    with self.assertRaisesRegex(self.resolver.ResolutionError, message):
                        self.resolver.resolve_system_model(path, {**document, "layers": references})

    def test_reordering_layers_preserves_content_and_records_selection_order(self):
        expected = self.real_model("origin200")
        actual = self.load_layer_changes([("origin200.yaml", lambda m: m["layers"].reverse())])
        expected["composition"]["layers"].reverse()
        self.assertEqual(actual, expected)

    def test_assembly_platform_is_required_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.address_composition(directory)
            assembly_path = pathlib.Path(directory) / "assembly.yaml"
            assembly = yaml.safe_load(assembly_path.read_text())
            assembly["layer"].pop("platform")
            assembly_path.write_text(yaml.safe_dump(assembly))
            with self.assertRaisesRegex(
                    self.resolver.ResolutionError, "no software platform identity"):
                self.resolver.load_system_model(path)

    def test_authoring_reuses_physical_module_layers(self):
        for path in sorted((INPUTS / "layers").glob("*/*.yaml")):
            layer = yaml.safe_load(path.read_text())["layer"]
            self.assertIn(layer["kind"], {
                "architecture", "assembly", "module", "product", "implementation",
            })
        dual = yaml.safe_load((INPUTS / "origin200-dual.yaml").read_text())
        self.assertEqual(
            [instance["model"] for instance in dual["module_instances"]],
            ["layers/modules/logic-carrier.yaml", "layers/modules/logic-carrier.yaml"],
        )

    def load_reference_population(self, change):
        model = self.real_model("origin2000")
        change(model)
        return self.reload_model(model)

    def connector_selection(self, component="xio-slot-3", **fields):
        return {
            "component": component, "occupancy": "empty",
            "provenance": "derived", "sources": ["Reference population fixture"],
            "reason": "Select an unpopulated optional slot.", **fields,
        }

    def test_connector_selection_preserves_inventory_evidence(self):
        selection = self.connector_selection()
        path = INPUTS / "origin2000.yaml"
        composition = yaml.safe_load(path.read_text())
        composition["connector_population"] = [selection]
        model = self.resolver.resolve_system_model(path, composition)
        self.assertEqual(model["composition"].get("connector_population"), [selection])
        connector = next(item for item in model["components"]
                         if item["id"] == "xio-slot-3")
        self.assertEqual(connector["connector"]["occupancy"], "opaque")

    def test_module_occupies_an_external_cable_connector(self):
        model = self.real_model("origin300")
        model["components"].extend([
            {"id": "graphics-brick", "name": "Graphics brick",
             "kind": "module", "parent": "xio-connector",
             "provenance": "derived", "sources": ["Synthetic composition fixture"]},
            {"id": "graphics-port", "name": "Graphics cable port",
             "kind": "connector", "parent": "graphics-brick",
             "connector": {"interface": "xio", "location": "rear",
                           "occupancy": "occupied"},
             "provenance": "derived", "sources": ["Synthetic composition fixture"]},
        ])
        model["connections"].append({
            "component": "graphics-port", "bus": "xio", "role": "endpoint",
            "provenance": "derived", "sources": ["Synthetic composition fixture"],
        })
        model["composition"]["connector_population"] = [
            {**selection, "occupancy": "occupied", "occupant": "graphics-brick"}
            if selection["component"] == "xio-connector" else selection
            for selection in model["composition"]["connector_population"]
        ]

        self.reload_model(model)

    def fit_board(self, model):
        model["components"].append({
            "id": "fitted-board", "name": "Fitted board", "kind": "board",
            "parent": "xio-slot-3", "provenance": "derived",
            "sources": ["Reference population fixture"],
        })
        model["connections"].append({
            "component": "fitted-board", "bus": "xbow0-io3",
            "role": "endpoint", "provenance": "derived",
            "sources": ["Reference population fixture"],
        })

    def test_connector_selection_rejects_invalid_bindings(self):
        def select(*selections):
            def change(model):
                model["composition"]["connector_population"] = list(selections)
            return change

        def known_population_conflict(model):
            connector = next(item for item in model["components"]
                             if item["id"] == "xio-slot-3")
            connector["connector"]["occupancy"] = "occupied"
            model["composition"]["connector_population"] = [
                self.connector_selection()]

        def reused_occupant(model):
            self.fit_board(model)
            model["composition"]["connector_population"] = [
                self.connector_selection(cid, occupancy="occupied",
                                         occupant="fitted-board")
                for cid in ("xio-slot-3", "xio-slot-4")]

        cases = (
            ("unknown connector", select(self.connector_selection("missing")),
             "unknown connector"),
            ("component is not a connector",
             select(self.connector_selection("node-board-1-hub")), "not a connector"),
            ("connector selected twice",
             select(self.connector_selection(), self.connector_selection()),
             "repeated"),
            ("empty connector names an occupant",
             select(self.connector_selection(occupant="baseio")),
             "empty.*occupant"),
            ("occupied connector names no occupant",
             select(self.connector_selection(occupancy="occupied")),
             "requires an occupant"),
            ("occupant is not a component",
             select(self.connector_selection(occupancy="occupied",
                                             occupant="missing")),
             "unknown occupant"),
            ("occupant is not attached to the connector",
             select(self.connector_selection(occupancy="occupied",
                                             occupant="baseio")),
             "not connected"),
            ("selection contradicts the known population",
             known_population_conflict, "conflicts"),
            ("one board occupies two connectors", reused_occupant,
             "occupant.*repeated"),
            ("occupancy is not a string",
             select(self.connector_selection(occupancy=[])),
             "invalid selection"),
            ("provenance is not a string",
             select(self.connector_selection(provenance=[])),
             "invalid selection"),
            ("a source is not a string",
             select(self.connector_selection(sources=[42])),
             "invalid selection"),
            ("a source is empty",
             select(self.connector_selection(sources=[""])),
             "invalid selection"),
        )
        for name, change, message in cases:
            with self.subTest(name=name):
                with self.assertRaisesRegex(
                        self.resolver.ResolutionError, message):
                    self.load_reference_population(change)

    def test_occupied_connector_requires_attached_board(self):
        def change(model):
            self.fit_board(model)
            model["composition"]["connector_population"] = [
                self.connector_selection(occupancy="occupied",
                                         occupant="fitted-board")
            ]
        model = self.load_reference_population(change)
        self.assertEqual(
            model["composition"]["connector_population"][0]["occupant"],
            "fitted-board")

    def test_implemented_routed_model_requires_connector_selections(self):
        model = self.real_model("origin2000-rack")
        model["machine"]["emulation"]["lifecycle"] = "implemented"
        del model["composition"]["connector_population"]
        with self.assertRaisesRegex(self.resolver.ResolutionError, "unresolved connector"):
            self.reload_model(model)

    def test_physical_slot_namespaces_allow_node_router_and_io_slot_one(self):
        def change(model):
            for cid, kind in (("node-board-1", "node"), ("router-board-1", "router"),
                              ("baseio", "io")):
                component = next(item for item in model["components"] if item["id"] == cid)
                component["physical_slot"] = {"kind": kind, "number": 1}
        self.load_reference_population(change)

    def test_physical_slot_rejects_duplicate_node_slot(self):
        def change(model):
            for component in model["components"]:
                if component["id"] in {"node-board-1", "node-board-2"}:
                    component["physical_slot"] = {"kind": "node", "number": 1}
        with self.assertRaisesRegex(self.resolver.ResolutionError, "physical slot.*repeated"):
            self.load_reference_population(change)

    def test_origin2000_published_internal_links_and_slots(self):
        model = self.real_model("origin2000")
        components = {item["id"]: item for item in model["components"]}
        for prefix, kind, count in (("node-board", "node", 4), ("router-board", "router", 2)):
            for number in range(1, count + 1):
                self.assertEqual(components[f"{prefix}-{number}"].get("physical_slot"),
                                 {"kind": kind, "number": number})
        self.assertEqual(components["baseio"].get("physical_slot"), {"kind": "io", "number": 1})
        expected = {
            "r1-node1": {("node-board-1-hub", None), ("router-1", 5)},
            "r1-node2": {("node-board-2-hub", None), ("router-1", 4)},
            "r2-node3": {("node-board-3-hub", None), ("router-2", 5)},
            "r2-node4": {("node-board-4-hub", None), ("router-2", 4)},
            "r1-r2": {("router-1", 6), ("router-2", 6)},
        }
        buses = {item["id"]: item for item in model["buses"]}
        for bus, endpoints in expected.items():
            self.assertEqual(buses[bus].get("link"), {"kind": "midplane"})
            self.assertEqual({(item["component"], item.get("fabric_port"))
                              for item in model["connections"] if item["bus"] == bus},
                             endpoints)

    def test_origin2000_published_crossbow_widgets(self):
        model = self.real_model("origin2000")
        crossbow_map = {
            item["bus"]: (item["xio"]["physical_port"], item["widget"])
            for item in model["connections"]
            if item["bus"].startswith("xbow")
        }
        self.assertEqual(
            crossbow_map,
            {
                "xbow0-io1": (0, 8),
                "xbow0-node1": (1, 9),
                "xbow0-node3": (2, 10),
                "xbow0-io4": (3, 11),
                "xbow0-io2": (4, 12),
                "xbow0-io6": (5, 13),
                "xbow0-io5": (6, 14),
                "xbow0-io3": (7, 15),
                "xbow1-io11": (0, 8),
                "xbow1-node4": (1, 9),
                "xbow1-node2": (2, 10),
                "xbow1-io8": (3, 11),
                "xbow1-io10": (4, 12),
                "xbow1-io12": (5, 13),
                "xbow1-io7": (6, 14),
                "xbow1-io9": (7, 15),
            },
        )

    def test_origin2000_module_has_concrete_physical_population(self):
        model = self.real_model("origin2000-rack")
        self.assertEqual(model["machine"]["model_role"], "emulation-target")
        population = model["composition"]["population"]
        self.assertEqual(population["cpus_per_node"], [2] * 4)
        self.assertEqual(population["memory_mib_per_node"], [256] * 4)
        self.assertEqual(population["memory_bank_mib_per_node"],
                         [[128, 128] + [0] * 6] * 4)
        connectors = {item["id"] for item in model["components"]
                      if item["kind"] == "connector"}
        selections = model["composition"]["connector_population"]
        self.assertEqual({item["component"] for item in selections}, connectors)
        # The stock module carries no graphics: every optional connector,
        # IO3 included, is empty until a board is fitted at launch.
        self.assertEqual(
            {item["component"]: item.get("occupant") for item in selections
             if item["occupancy"] != "empty"},
            {})
        self.assertNotIn("config_graph", model)
        self.assertNotIn("config_graphs", model)

    def test_selected_module_number_resolves_explicitly(self):
        model = self.real_model("origin2000-rack")
        policy = model["machine"]["emulation"].get("initial_module_numbers")
        self.assertIsInstance(policy, list)
        self.assertEqual([(entry["component"], entry["number"]) for entry in policy],
                         [("compute-module", 1)])

    def test_module_number_policy_rejections(self):
        cases = (
            ("policy missing from a routed target", "origin2000-rack",
             lambda emulation: emulation.pop("initial_module_numbers", None),
             r"initial_module_numbers"),
            ("a router-less target's policy is checked too", "origin200",
             lambda emulation: emulation.update(initial_module_numbers=[
                 {"component": "missing", "number": 1, "reason": "fixture"}]),
             r"unknown component 'missing'"),
            ("policy on a planned target", "origin2000-rack",
             lambda emulation: emulation.update(lifecycle="planned"),
             r"implemented emulation target"),
        )
        for name, machine, change, message in cases:
            with self.subTest(name=name):
                model = self.real_model(machine)
                change(model["machine"]["emulation"])
                with self.assertRaisesRegex(
                        self.resolver.ResolutionError, message):
                    self.reload_model(model)

    def test_initial_module_number_policy_requires_implemented_target(self):
        source = self.real_model("origin2000-rack")
        for role, lifecycle in (("emulation-target", "planned"),
                                ("reference", "implemented")):
            with self.subTest(role=role, lifecycle=lifecycle):
                model = copy.deepcopy(source)
                model["machine"]["model_role"] = role
                model["machine"]["emulation"]["lifecycle"] = lifecycle
                with self.assertRaisesRegex(self.resolver.ResolutionError,
                                            "implemented emulation target"):
                    self.resolver.model_composition.validate_initial_module_numbers(model)
                del model["machine"]["emulation"]["initial_module_numbers"]
                self.resolver.model_composition.validate_initial_module_numbers(model)

    def test_router_less_target_may_select_a_module_number(self):
        # A router-less Origin 2000 still has a module and its controller;
        # it may select the number, and the same rules check it, but only a
        # routed target must.
        model = self.real_model("origin2000-rack")
        model["components"] = [item for item in model["components"]
                               if item.get("kind") != "router-asic"]
        self.assertNotEqual(
            self.resolver.model_composition._fabric_topology(model), "router")
        self.resolver.model_composition.validate_initial_module_numbers(model)
        model["machine"]["emulation"]["initial_module_numbers"][0]["number"] = 0
        with self.assertRaisesRegex(self.resolver.ResolutionError, "1 through 255"):
            self.resolver.model_composition.validate_initial_module_numbers(model)
        del model["machine"]["emulation"]["initial_module_numbers"]
        self.resolver.model_composition.validate_initial_module_numbers(model)

    def test_module_number_bindings_cover_outer_modules(self):
        source = self.real_model("origin2000-rack")
        graph = self.resolver._instantiate_module_graph(
            source, prefix="second-", parent="rack",
            omit_buses=[], module_index=1,
            observed_hardware={
                "mode": "projected",
                "sources": ["second module duplicates the first"],
                "note": "Second module reuses the template; its BaseIO-G "
                        "MediaIO IOC3 placement is a product-symmetry projection.",
            })
        self.assertNotIn("machine", graph)
        for number, error in ((1, "repeated.*number"), (2, None)):
            model = copy.deepcopy(source)
            model["components"].append({"id": "second-module", "kind": "module"})
            model["machine"]["emulation"]["initial_module_numbers"] = [
                {"component": "second-module", "number": number, "reason": "Second installation."},
                {"component": "compute-module", "number": 1, "reason": "First installation."}]
            if error:
                with self.assertRaisesRegex(self.resolver.ResolutionError, error):
                    self.reload_model(model)
            else:
                resolved = self.reload_model(model)
                self.assertEqual(resolved["machine"]["emulation"]["initial_module_numbers"],
                                 model["machine"]["emulation"]["initial_module_numbers"])

    def routed_identity_model(self):
        model = self.real_model("origin2000-rack")
        model["machine"]["model_role"] = "emulation-target"
        model["composition"]["topology"]["supported_cpu_populations"] = [[2] * 4]
        for component in model["components"]:
            if component["id"] == "baseio-ioc3":
                component["hardware_kind"] = "ioc3"
        model["machine"]["emulation"] = {"profile": "origin2000-rack", "lifecycle": "planned"}
        identities = []
        for component in model["components"]:
            if (component["kind"] in {"node-asic", "router-asic", "pci-host-bridge"}
                    or component.get("hardware_kind") in {"ioc3", "hq4"}):
                identities.append({"component": component["id"],
                                   "serial": hex(len(identities) + 1),
                                   "reason": "Assign distinct synthetic fixture identities."})
        model["machine"]["emulation"]["synthetic_identities"] = identities
        return model

    def test_routed_synthetic_identities_require_complete_unique_literals(self):
        for mutation, error in (
                (lambda entries: entries.pop(), "missing synthetic identity"),
                (lambda entries: entries.append(dict(entries[0])), "repeated component"),
                (lambda entries: entries[1].update(serial=entries[0]["serial"]), "repeated serial"),
                (lambda entries: entries[0].update(component="missing"), "unknown component"),
                (lambda entries: entries[0].update(component="node-board-1-cpu-a"), "identity-bearing"),
                *[(lambda entries, serial=serial: entries[0].update(serial=serial), "serial")
                  for serial in (True, 1.0, 1, "1", "0x_1", "0x0", "0x1000000000000")]):
            with self.subTest(error=error, mutation=mutation):
                model = self.routed_identity_model()
                mutation(model["machine"]["emulation"]["synthetic_identities"])
                with self.assertRaisesRegex(self.resolver.ResolutionError, error):
                    self.reload_model(model)

    def test_routed_synthetic_identities_accept_complete_policy(self):
        model = self.routed_identity_model()
        resolved = self.reload_model(model)
        self.assertEqual(resolved["machine"]["emulation"]["synthetic_identities"],
                         model["machine"]["emulation"]["synthetic_identities"])

    def test_routed_two_node_target_does_not_require_firmware_graph(self):
        model = self.routed_identity_model()
        removed = {"node-board-3", "node-board-4", "router-board-1", "router-board-2"}
        while True:
            children = {item["id"] for item in model["components"]
                        if item.get("parent") in removed}
            if children <= removed:
                break
            removed.update(children)
        model["components"] = [item for item in model["components"] if item["id"] not in removed]
        model["buses"] = [item for item in model["buses"]
                          if item["kind"] != "fabric" and item.get("master") not in removed]
        for bus in model["buses"]:
            if "masters" in bus:
                bus["masters"] = [master for master in bus["masters"]
                                  if master not in removed]
        buses = {item["id"] for item in model["buses"]}
        model["connections"] = [item for item in model["connections"]
                                if item["component"] not in removed and item["bus"] in buses]
        model["buses"].append({"id": "null-router-link", "name": "Null Router link",
                               "kind": "fabric", "link": {"kind": "null-router"},
                               "provenance": "derived", "sources": ["Synthetic resolver fixture."]})
        model["connections"].extend({"component": f"hub-{number}", "bus": "null-router-link",
                                      "role": "endpoint", "provenance": "derived",
                                      "sources": ["Synthetic resolver fixture."]}
                                     for number in (1, 2))
        model["composition"]["connector_population"] = [
            item for item in model["composition"]["connector_population"]
            if item["component"] not in removed]
        emulation = model["machine"]["emulation"]
        emulation["synthetic_identities"] = [item for item in emulation["synthetic_identities"]
                                              if item["component"] not in removed]
        population = model["composition"]["population"]
        population["nodes"] = 2
        for field in ("cpus_per_node", "memory_mib_per_node", "memory_bank_mib_per_node", "dimms_per_node"):
            population[field] = population[field][:2]
        model["composition"]["topology"]["supported_cpu_populations"] = [[2, 2]]
        model["composition"]["topology"]["min_nodes"] = 2
        resolved = self.reload_model(model)
        self.assertNotIn("config_graphs", resolved)

    def test_memory_population_rejects_invalid_banks(self):
        for banks, message in (
                ([256, 128, 0, 0], r"bank sizes total 384 MiB, but population declares 512 MiB"),
                ([256, 0, 256, 0], r"memory banks must populate one contiguous run")):
            with self.subTest(banks=banks), tempfile.TemporaryDirectory() as directory:
                path = self.address_composition(directory, capacity_metadata={
                    "memory_banks_per_node": 4, "dimms_per_bank": 2,
                    "supported_bank_sizes_mib": [64, 128, 256, 512],
                })
                product_path = pathlib.Path(directory) / "product.yaml"
                product = yaml.safe_load(product_path.read_text())
                product["layer"]["population"].update(
                    dimms_per_node=[4], memory_mib_per_node=[512],
                    memory_bank_mib_per_node=[banks])
                product_path.write_text(yaml.safe_dump(product))
                with self.assertRaisesRegex(self.resolver.ResolutionError, message):
                    self.resolver.load_system_model(path)

    def test_memory_banks_on_one_dimm_pair_are_fitted_alike(self):
        capacity = {"memory_banks_per_node": 4, "dimms_per_bank": 1,
                    "banks_per_dimm_pair": 2,
                    "supported_bank_sizes_mib": [128, 256]}
        for banks, extra, message in (
                ([256, 128, 0, 0], {},
                 "node 0 banks 0 to 1 share DIMM pair 0 and must be fitted "
                 "alike"),
                ([256, 256, 128, 0], {},
                 "node 0 banks 2 to 3 share DIMM pair 1 and must be fitted "
                 "alike"),
                ([128, 128, 128, 128], {"banks_per_dimm_pair": 3},
                 "assembly 'ip29' banks_per_dimm_pair must divide its 4 "
                 "memory banks"),
                ([256, 256, 128, 128], {}, None),
        ):
            with self.subTest(banks=banks), \
                    tempfile.TemporaryDirectory() as directory:
                path = self.address_composition(
                    directory, capacity_metadata={**capacity, **extra})
                product_path = pathlib.Path(directory) / "product.yaml"
                product = yaml.safe_load(product_path.read_text())
                product["layer"]["population"].update(
                    dimms_per_node=[sum(1 for size in banks if size)],
                    memory_mib_per_node=[sum(banks)],
                    memory_bank_mib_per_node=[banks])
                product_path.write_text(yaml.safe_dump(product))
                if message is None:
                    model = self.resolver.load_system_model(path)
                    self.assertEqual(
                        model["composition"]["population"]
                        ["memory_bank_mib_per_node"], [banks])
                    continue
                with self.assertRaisesRegex(
                        self.resolver.ResolutionError, message):
                    self.resolver.load_system_model(path)

    def test_product_narrows_its_processors_and_bank_rows(self):
        product = "layers/products/origin300.yaml"

        def topology(**fields):
            def change(document):
                document["layer"]["topology"].update(fields)
            return change

        fitted = [0, 0, 256, 256, 0, 0, 0, 0]
        model = self.load_layer_changes([(product, topology(
            supported_processors=[
                {"part": "r14000", "clock_mhz": 500, "cache_mib": 2}],
            supported_bank_mib_per_node=[fitted]))], name="origin300")
        self.assertEqual(
            model["composition"]["topology"]["supported_bank_mib_per_node"],
            [fitted])
        for fields, message in (
                ({"supported_processors": [
                    {"part": "r14000", "clock_mhz": 600, "cache_mib": 2}]},
                 "product 'origin300' does not support CPU 'cpu0', r14000 "
                 "at 500 MHz with 2 MiB of cache"),
                ({"supported_processors": [{"part": "r14000"}]},
                 "supported_processors entries must name part, clock_mhz "
                 "and cache_mib"),
                ({"supported_bank_mib_per_node": [
                    [0, 0, 512, 512, 0, 0, 0, 0]]},
                 r"product 'origin300' does not support node 0 memory banks "
                 r"\[0, 0, 256, 256, 0, 0, 0, 0\]"),
                ({"supported_bank_mib_per_node": []},
                 "supported_bank_mib_per_node needs a non-empty list"),
        ):
            with self.subTest(fields=fields), self.assertRaisesRegex(
                    self.resolver.ResolutionError, message):
                self.load_layer_changes([(product, topology(**fields))],
                                        name="origin300")

    def test_origin200_resolution_is_deterministic_and_complete(self):
        # Two independent fresh loads of the same real file, not real_model()'s
        # single cached copy: this is what tests the loader's own determinism.
        path = INPUTS / "origin200.yaml"
        first = self.resolver.load_system_model(path)
        second = self.resolver.load_system_model(path)

        self.assertEqual(
            json.dumps(first, sort_keys=True, separators=(",", ":")),
            json.dumps(second, sort_keys=True, separators=(",", ":")),
        )
        self.assertEqual(first["composition"]["architecture"], "sn0")
        self.assertEqual(first["composition"]["platform"], "ip27")
        self.assertEqual(first["composition"]["assembly"], "ip29")
        self.assertEqual(first["composition"]["product"], "origin200")
        self.assertEqual(first["composition"]["population"]["nodes"], 1)
        self.assertEqual(first["composition"]["limits"]["nodes"], 1)
        self.assertEqual(first["composition"]["limits"]["cpus_per_node"], 2)
        self.assertEqual(first["composition"]["limits"]["dimms_per_node"], 8)
        self.assertEqual(first["composition"]["limits"]["memory_banks_per_node"], 4)
        self.assertEqual(first["composition"]["population"]["dimms_per_node"], [2])
        self.assertEqual(
            first["composition"]["population"]["memory_bank_mib_per_node"],
            [[64, 0, 0, 0]],
        )
        self.assertEqual(first["machine"]["emulation"]["machine_name"], "origin200")
        self.assertEqual(first["machine"]["model_role"], "emulation-target")
        self.assertTrue(first["components"])
        self.assertTrue(first["buses"])
        self.assertTrue(first["connections"])
        self.assert_no_emulation(first["address_spaces"])

    def test_public_loader_rejects_documents_that_are_not_compositions(self):
        cases = (
            ("resolved model", self.real_model("origin300")),
            ("KLCONFIG layout", {"layout_version": 1, "records": {}}),
        )
        for name, document in cases:
            with self.subTest(name=name), \
                    tempfile.TemporaryDirectory() as directory:
                path = pathlib.Path(directory) / "document.yaml"
                path.write_text(yaml.safe_dump(document))
                with self.assertRaisesRegex(
                        self.resolver.ResolutionError,
                        "system model input must be a layered composition"):
                    self.resolver.load_system_model(path)

    def test_compile_checks_processor_population(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        from sgi_model.model_compile import compile_model
        model = self.real_model("origin200")
        model["components"] = [
            component for component in model["components"]
            if component.get("kind") != "cpu"
        ]
        with self.assertRaisesRegex(ValueError, "has no populated CPU component"):
            compile_model(model)

    def test_composed_capacity_evidence_is_preserved_in_resolved_limits(self):
        metadata = {
            "provenance": "vendor-document",
            "sources": ["Assembly capacity fixture"],
            "evidence_gap": "The maximum fitted capacity remains open.",
        }
        with tempfile.TemporaryDirectory() as directory:
            model = self.resolver.load_system_model(
                self.address_composition(directory, capacity_metadata=metadata)
            )

        for field, value in metadata.items():
            self.assertEqual(model["composition"]["limits"][field], value)
    def test_absolute_layer_reference_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "model.yaml"
            path.write_text(yaml.safe_dump({
                "configuration_version": 1,
                "layers": ["/tmp/not-a-hardware-model.yaml"],
            }))

            with self.assertRaisesRegex(
                    self.resolver.ResolutionError,
                    r"absolute layer reference"):
                self.resolver.load_system_model(path)

    def test_shared_hardware_address_space_resolves_for_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            model = self.resolver.load_system_model(
                self.address_composition(directory)
            )

        self.assert_no_emulation(model)
        self.assertEqual(model["address_spaces"][0]["selector"], 1)

    def test_product_exclusion_rejects_appended_component(self):
        exclusion = {
            "match": {"kind": "crossbar"},
            "reason": "The base product has a direct Hub-to-Bridge link.",
            "provenance": "vendor-document",
            "sources": ["Base product topology fixture"],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = self.address_composition(directory)
            product_path = pathlib.Path(directory) / "product.yaml"
            product = yaml.safe_load(product_path.read_text())
            product["layer"]["excluded_components"] = [exclusion]
            product["layer"]["data"]["components"].append({
                "id": "crossbow",
                "name": "Crossbow",
                "kind": "crossbar",
                "provenance": "vendor-document",
            })
            product_path.write_text(yaml.safe_dump(product, sort_keys=False))

            with self.assertRaisesRegex(
                    self.resolver.ResolutionError,
                    r"product 'address-product' excludes component 'crossbow'.*kind='crossbar'"):
                self.resolver.load_system_model(path)

    def test_exclusion_validation_preserves_owners_and_copies(self):
        exclusion = {"match": {"id": "node"}, "reason": "Not installed", "sources": ["fixture"]}
        for declarations, components, message in (
                ([("a", [])], None, "a excluded_components must be a non-empty list"),
                ([("a", {})], {}, "a excluded_components must be a non-empty list"),
                ([("a", [{}])], None, "match must select"),
                ([("a", [dict(exclusion, reason="")])], None, "requires a reason"),
                ([("a", [exclusion]), ("b", [exclusion])], None, "in a and b"),
                ([("a", [exclusion, {}])], [{"id": "node"}], "a excludes component 'node'")):
            with self.subTest(declarations=declarations), self.assertRaisesRegex(
                    self.resolver.ResolutionError, message):
                self.resolver.model_composition.component_exclusions(declarations, components)
        copied = self.resolver.model_composition.component_exclusions([("a", [exclusion])], None)
        copied[0]["sources"].append("changed")
        self.assertEqual(exclusion["sources"], ["fixture"])

    def test_assembly_exclusion_rejects_product_component(self):
        exclusion = {
            "match": {"id": "base-io-board"},
            "reason": "This expansion inventory has no Origin 2000 BaseIO board.",
            "provenance": "vendor-document",
            "sources": ["Expansion inventory topology fixture"],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = self.address_composition(directory)
            assembly_path = pathlib.Path(directory) / "assembly.yaml"
            assembly = yaml.safe_load(assembly_path.read_text())
            assembly["layer"]["excluded_components"] = [exclusion]
            assembly_path.write_text(yaml.safe_dump(assembly, sort_keys=False))
            product_path = pathlib.Path(directory) / "product.yaml"
            product = yaml.safe_load(product_path.read_text())
            product["layer"]["data"]["components"].append({
                "id": "base-io-board",
                "name": "BaseIO board",
                "kind": "board",
                "provenance": "vendor-document",
            })
            product_path.write_text(yaml.safe_dump(product, sort_keys=False))

            with self.assertRaisesRegex(
                    self.resolver.ResolutionError,
                    r"assembly 'ip29' excludes component 'base-io-board'.*id='base-io-board'"):
                self.resolver.load_system_model(path)

    def test_layer_exclusions_are_preserved_deterministically(self):
        exclusion = {
            "match": {"kind": "crossbar"},
            "reason": "The product has no crossbar.",
            "provenance": "vendor-document",
            "sources": ["Product topology fixture"],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = self.address_composition(directory)
            product_path = pathlib.Path(directory) / "product.yaml"
            product = yaml.safe_load(product_path.read_text())
            product["layer"]["excluded_components"] = [exclusion]
            product_path.write_text(yaml.safe_dump(product, sort_keys=False))

            first = self.resolver.load_system_model(path)
            second = self.resolver.load_system_model(path)

        self.assertEqual(first["excluded_components"], [exclusion])
        self.assertEqual(first, second)

    def test_equivalent_layer_exclusion_selectors_are_rejected(self):
        exclusion = {
            "match": {"kind": "crossbar"},
            "reason": "The product has no crossbar.",
        }
        with tempfile.TemporaryDirectory() as directory:
            path = self.address_composition(directory)
            product_path = pathlib.Path(directory) / "product.yaml"
            product = yaml.safe_load(product_path.read_text())
            product["layer"]["excluded_components"] = [exclusion, dict(exclusion)]
            product_path.write_text(yaml.safe_dump(product, sort_keys=False))

            with self.assertRaisesRegex(
                    self.resolver.ResolutionError,
                    r"duplicate excluded_components selector kind='crossbar'"):
                self.resolver.load_system_model(path)

    def test_checked_in_sn0_architecture_layer_is_hardware_only(self):
        layer = yaml.safe_load((
            INPUTS / "layers/architectures/sn0.yaml"
        ).read_text())

        self.assert_no_emulation(layer)

    def test_resolved_reference_rejects_emulation_state(self):
        model = self.real_model("origin2000")
        model["machine"]["emulation"] = {"lifecycle": "planned"}
        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                r"reference model cannot contain implementation state"):
            self.reload_model(model)

    def test_module_instance_prefixes_graph_references_and_attaches_root(self):
        contract = {
            "machine": {"name": "ignored source machine"},
            "components": [
                {"id": "chassis", "name": "Chassis", "kind": "chassis"},
                {"id": "hub", "name": "Hub", "kind": "node-asic",
                 "parent": "chassis",
                 "regions": [{"name": "local", "relative_to": "chassis"}]},
            ],
            "buses": [
                {"id": "xio", "name": "XIO", "kind": "xio",
                 "master": "hub"},
                {"id": "craylink", "name": "CrayLink", "kind": "fabric",
                 "master": "hub"},
            ],
            "connections": [
                {"component": "hub", "bus": "xio", "role": "master"},
                {"component": "hub", "bus": "craylink", "role": "master"},
                {"component": "hub", "bus": "xio", "role": "endpoint",
                 "controlled_by": "hub"},
            ],
            "interrupts": [{
                "id": "hub-error",
                "source": "hub",
                "chain": [{"at": "hub", "as": "error pending"}],
            }],
            "open_questions": [{
                "id": "hub-link",
                "question": "Which port carries the link?",
                "about": ["hub"],
                "candidates": [
                    {"answer": "port A", "source": "fixture"},
                    {"answer": "port B", "source": "fixture"},
                ],
            }],
        }

        for field in ("root", "instance_version"):
            with self.assertRaisesRegex(self.resolver.ResolutionError, f"unsupported field '{field}'"):
                self.load_module(contract, **{field: 1})
        graph = self.load_module(contract, omit_buses=["craylink"])

        self.assertEqual(graph["machine"]["name"], "Address projection fixture")
        self.assertEqual(graph["components"][1]["id"], "node0-chassis")
        self.assertEqual(graph["components"][1]["parent"], "hub")
        self.assertEqual(graph["components"][2]["parent"], "node0-chassis")
        self.assertEqual(
            graph["components"][2]["regions"][0]["relative_to"],
            "node0-chassis",
        )
        self.assertEqual([bus["id"] for bus in graph["buses"]], ["xio", "node0-xio"])
        self.assertEqual(graph["buses"][1]["master"], "node0-hub")
        self.assertEqual(graph["connections"][1:], [{
            "component": "node0-hub",
            "bus": "node0-xio",
            "role": "master",
        }, {
            "component": "node0-hub",
            "bus": "node0-xio",
            "role": "endpoint",
            "controlled_by": "node0-hub",
        }])
        self.assertEqual(graph["interrupts"][0]["source"], "node0-hub")
        self.assertEqual(graph["interrupts"][0]["chain"][0]["at"], "node0-hub")
        self.assertEqual(graph["open_questions"][0]["about"], ["node0-hub"])

    def test_nested_module_instances_prefix_interior_and_override_root_id(self):
        sub_model = {
            "components": [
                {"id": "board", "name": "Board", "kind": "board"},
                {"id": "hub", "name": "Hub", "kind": "node-asic",
                 "parent": "board",
                 "regions": [{"name": "local", "relative_to": "board"}]},
            ],
            "buses": [{"id": "sysad", "name": "SysAD", "kind": "sysad", "master": "hub"}],
            "connections": [{"component": "hub", "bus": "sysad", "role": "endpoint"}],
            "interrupts": [{"id": "hub-error", "source": "hub",
                            "chain": [{"at": "hub", "as": "error pending"}]}],
            "open_questions": [{"id": "hub-link", "question": "Which port?",
                                "about": ["hub"],
                                "candidates": [{"answer": "A", "source": "fixture"}]}],
        }
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = self.module_composition(root / "board", sub_model)
            data = {
                "components": [{"id": "chassis", "name": "Chassis", "kind": "chassis"}],
                "buses": [], "connections": [], "interrupts": [], "open_questions": [],
                "module_instances": [
                    {"model": source.relative_to(root).as_posix(), "id": "board-1",
                     "parent": "chassis", "physical_slot": {"kind": "node", "number": 1}},
                    {"model": source.relative_to(root).as_posix(), "id": "board-2",
                     "parent": "chassis", "physical_slot": {"kind": "node", "number": 2}},
                ],
            }
            graph = self.resolver._expand_nested_module_instances(root / "outer.yaml", data)

        by_id = {component["id"]: component for component in graph["components"]}
        self.assertEqual(sorted(by_id), sorted(
            ["chassis", "board-1", "board-2", "board-1-hub", "board-2-hub"]))
        self.assertEqual(by_id["board-1"]["parent"], "chassis")
        self.assertEqual(by_id["board-1"]["physical_slot"], {"kind": "node", "number": 1})
        self.assertEqual(by_id["board-2"]["physical_slot"], {"kind": "node", "number": 2})
        self.assertEqual(by_id["board-1-hub"]["parent"], "board-1")
        self.assertEqual(by_id["board-1-hub"]["regions"][0]["relative_to"], "board-1")
        self.assertEqual([bus["id"] for bus in graph["buses"]],
                          ["board-1-sysad", "board-2-sysad"])
        self.assertEqual(graph["buses"][0]["master"], "board-1-hub")
        self.assertEqual(graph["connections"][0],
                          {"component": "board-1-hub", "bus": "board-1-sysad", "role": "endpoint"})
        self.assertEqual(graph["interrupts"][0]["source"], "board-1-hub")
        self.assertEqual(graph["interrupts"][0]["chain"][0]["at"], "board-1-hub")
        self.assertEqual(graph["open_questions"][0]["id"], "board-1-hub-link")
        self.assertEqual(graph["open_questions"][0]["about"], ["board-1-hub"])
        self.assertNotIn("module_instances", graph)

    def test_nested_module_instances_rejects_invalid_declarations(self):
        sub_model = {"components": [{"id": "board", "name": "Board", "kind": "board"}]}
        slotted_sub_model = {"components": [
            {"id": "board", "name": "Board", "kind": "board",
             "physical_slot": {"kind": "node", "number": 9}},
        ]}
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = self.module_composition(root / "board", sub_model)
            slotted_source = self.module_composition(root / "slotted-board", slotted_sub_model)
            model = source.relative_to(root).as_posix()
            base = {"model": model, "id": "board-1", "parent": "chassis",
                    "physical_slot": {"kind": "node", "number": 1}}

            def data_for(**override):
                return {
                    "components": [{"id": "chassis", "name": "Chassis", "kind": "chassis"}],
                    "buses": [], "connections": [], "interrupts": [], "open_questions": [],
                    "module_instances": [{**base, **override}],
                }

            outer = root / "outer.yaml"
            cases = (
                ("not a list", "not-a-list", r"must be a non-empty list"),
                ("empty list", [], r"must be a non-empty list"),
            )
            for name, value, message in cases:
                with self.subTest(name=name), self.assertRaisesRegex(
                        self.resolver.ResolutionError, message):
                    self.resolver._expand_nested_module_instances(
                        outer, {**data_for(), "module_instances": value})

            field_cases = (
                ("unknown field", {"role": "MASTER"}, r"unsupported field 'role'"),
                ("missing model", {"model": ""}, r"requires non-empty model, id and parent"),
                ("missing physical_slot", {"physical_slot": {}}, r"requires a physical_slot"),
                ("unknown parent", {"parent": "missing"}, r"names unknown enclosing parent"),
                ("model outside directory", {"model": "../outside.yaml"},
                 r"outside its composition directory"),
                ("model is the module itself", {"model": "outer.yaml"},
                 r"recursive module-instance composition"),
                ("missing model file", {"model": "missing.yaml"},
                 r"missing module-instance model"),
            )
            for name, override, message in field_cases:
                with self.subTest(name=name), self.assertRaisesRegex(
                        self.resolver.ResolutionError, message):
                    self.resolver._expand_nested_module_instances(outer, data_for(**override))

            with self.subTest(name="duplicate id"), self.assertRaisesRegex(
                    self.resolver.ResolutionError, r"reuses component id 'board-1'"):
                data = data_for()
                data["module_instances"].append(dict(base))
                self.resolver._expand_nested_module_instances(outer, data)

            with self.subTest(name="sub-layer already authors physical_slot"), \
                    self.assertRaisesRegex(
                        self.resolver.ResolutionError,
                        r"already authors one on its root component"):
                self.resolver._expand_nested_module_instances(outer, data_for(
                    model=slotted_source.relative_to(root).as_posix()))

    def test_module_instance_capture_basis_controls_provenance(self):
        contract = {"components": [{
            "id": "chassis", "name": "Chassis", "kind": "chassis",
            "provenance": "observed-hardware", "sources": ["source module capture"],
        }]}
        for mode, source, provenance in (
                ("direct", "instance capture", "observed-hardware"),
                ("projected", "symmetric product drawing", "derived")):
            with self.subTest(mode=mode):
                model = self.load_module(contract, observed_hardware={
                    "mode": mode, "sources": [source], "note": "Fixture capture basis.",
                })
                component = next(item for item in model["components"]
                                 if item["id"] == "node0-chassis")
                self.assertEqual(component["provenance"], provenance)
                self.assertEqual(component["sources"], ["source module capture", source])

    def test_origin200_dual_instance_basis_uses_catalogued_capture_ids(self):
        model = self.real_model("origin200-dual")
        sources = "\n".join(
            source
            for connection in model["connections"]
            for source in connection.get("sources", [])
        )

        self.assertIn(
            "bce07195c21b8750189c22adb5b68a525fc60ab8f113105b1765a59cc740060f",
            sources,
        )
        self.assertNotIn("b6a76788b894308b8c46f217acca3e63af3bd7e9", sources)
        self.assertNotIn(
            "bce829db7275946a0204b5c34fd9a2144d9514b290229645766cc48feb72b5a1",
            sources,
        )
        self.assertNotIn("b6a611d92814bc210b084e973518f8b84c40d9bb", sources)

    def test_emulation_target_requires_valid_emulation_profile(self):
        model = self.real_model("origin200")
        model["machine"]["emulation"].pop("profile", None)
        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                r"emulation target requires a profile identifier"):
            self.reload_model(model)

        model["machine"]["emulation"]["profile"] = "Dual Module"
        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                r"profile identifier"):
            self.reload_model(model)

    def test_origin200_emulation_populations_are_profile_scoped(self):
        base = self.real_model("origin200")
        dual = self.real_model("origin200-dual")

        self.assertEqual(base["machine"]["emulation"]["profile"], "base")
        self.assertEqual(dual["machine"]["emulation"]["profile"], "dual-module")
        self.assertEqual(
            base["composition"]["topology"]["supported_cpu_populations"],
            [[1], [2]],
        )
        self.assertEqual(
            dual["composition"]["population"]["cpus_per_node"], [2, 2]
        )

    def test_origin200_profiles_agree_on_shared_board_facts(self):
        base = self.real_model("origin200")
        dual = self.real_model("origin200-dual")

        self.assertNotIn("implementation", base["composition"])
        self.assertNotIn("implementation", dual["composition"])
        for field in (
                "machine_name", "display_description", "lifecycle",
                "emulator_constraints"):
            with self.subTest(field=field):
                self.assertEqual(
                    base["machine"]["emulation"][field],
                    dual["machine"]["emulation"][field],
                )
        self.assertEqual(
            base["machine"]["firmware_config"],
            dual["machine"]["firmware_config"],
        )

    def test_origin200_resolves_direct_static_facts(self):
        model = self.real_model("origin200")
        emulation = model["machine"]["emulation"]
        self.assertNotIn("semantic_bindings", emulation)
        self.assertNotIn("runtime_topology", emulation)
        junk_uart = next(
            component for component in model["components"]
            if component["id"] == "junk-uart"
        )
        self.assertEqual(
            junk_uart["optional"],
            {"node": 0, "default_enabled": False},
        )

        hub_sysad = next(
            connection for connection in model["connections"]
            if connection.get("component") == "hub"
            and connection.get("bus") == "sysad"
        )
        self.assertEqual(hub_sysad["cpu_interrupt_inputs"], {
            "bank0": 0, "bank1": 1, "rt": 2,
        })
        i2c = next(bus for bus in model["buses"] if bus["id"] == "i2c")
        self.assertEqual(i2c["master"], "pcf8584")
        i2c_elsc = next(
            connection for connection in model["connections"]
            if connection.get("component") == "sgi-elsc"
            and connection.get("bus") == "i2c"
        )
        self.assertEqual(i2c_elsc["i2c_address"], 0x08)
        i2c_nvram = next(
            connection for connection in model["connections"]
            if connection.get("component") == "i2c-nvram"
            and connection.get("bus") == "i2c"
        )
        self.assertEqual(i2c_nvram["i2c_address"], 0x50)
        self.assertEqual(i2c_nvram["i2c_address_count"], 8)
        board_identity = next(
            component for component in model["components"]
            if component["id"] == "board-identity"
        )
        self.assertEqual(board_identity["identity"]["part"], "030-1244-001")

    def _model_with_pic_profile(self, owner="fixture-pic",
                                 profile="pic-rev3-bedrock"):
        model = self.real_model("origin300")
        model["components"].append({
            "id": "fixture-pic", "name": "Synthetic PIC",
            "kind": "pci-host-bridge", "hardware_kind": "pic",
            "pci_host_bridge": {"heads": 2}, "provenance": "derived",
        })
        model["machine"]["emulation"]["pic_profiles"] = {owner: profile}
        return model

    def test_pic_profile_map_accepts_one_explicit_pic_owner(self):
        model = self._model_with_pic_profile()

        resolved = self.reload_model(model)

        self.assertEqual(resolved["machine"]["emulation"]["pic_profiles"], {
            "fixture-pic": "pic-rev3-bedrock",
        })

    def test_pic_profile_map_rejects_unknown_and_non_pic_owners(self):
        for owner, message in (
                ("missing-pic", "pic_profiles names unknown component"),
                ("bedrock", "is not a PIC host")):
            with self.subTest(owner=owner):
                model = self._model_with_pic_profile(owner=owner)
                with self.assertRaisesRegex(
                        self.resolver.ResolutionError, message):
                    self.reload_model(model)

    def test_pic_profile_map_rejects_unsupported_profile_values(self):
        model = self._model_with_pic_profile(profile="pic-rev4-bedrock")

        with self.assertRaisesRegex(
                self.resolver.ResolutionError, "pic_profiles"):
            self.reload_model(model)

    def test_pic_profiles_follow_module_prefixes_and_omitted_owners(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            module = root / "module.yaml"
            module.write_text(yaml.safe_dump({
                "layer_version": 1,
                "layer": {
                    "kind": "module", "id": "pic-module",
                    "requires": {"assembly": "ip29"},
                    "data": {"components": [
                        {"id": "board", "kind": "board"},
                        {"id": "pic", "kind": "pci-host-bridge",
                         "hardware_kind": "pic", "parent": "board",
                         "optional": {"node": 1},
                         "pci_host_bridge": {"heads": 1}},
                    ]},
                },
            }))
            resolved = {
                "machine": {"emulation": {"pic_profiles": {
                    "first-pic": "pic-rev3-bedrock",
                    "second-pic": "pic-rev3-bedrock",
                    "missing-pic": "pic-rev3-bedrock",
                }}},
                "components": [{"id": "system", "kind": "chassis"}],
                "buses": [], "connections": [], "interrupts": [],
                "open_questions": [],
            }
            self.resolver._merge_module_instances(
                root / "system.yaml", {"module_instances": [
                    {"model": "module.yaml", "prefix": "first-",
                     "parent": "system"},
                    {"model": "module.yaml", "prefix": "second-",
                     "parent": "system"},
                ]}, resolved,
                {"product": {"kind": "product", "id": "fixture"},
                 "assembly": {"kind": "assembly", "id": "ip29"}},
            )

        self.assertEqual(resolved["machine"]["emulation"]["pic_profiles"], {
            "second-pic": "pic-rev3-bedrock",
            "missing-pic": "pic-rev3-bedrock",
        })
        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                "pic_profiles names unknown component 'missing-pic'"):
            self.resolver.model_composition.validate_initialization_policy(
                resolved)

    def test_optional_component_must_target_a_supported_profile_node(self):
        model = self.real_model("origin200")
        model["components"].append({
            "id": "unsupported-uart",
            "name": "unsupported UART",
            "kind": "subfunction",
            "provenance": "measured-on-our-model",
            "optional": {"node": 1, "default_enabled": False},
        })

        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                r"optional component 'unsupported-uart' names node 1 outside "
                r"every supported profile"):
            self.reload_model(model)

    def test_layer_contracts_supply_machine_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.address_composition(directory)
            root = pathlib.Path(directory)
            contracts = {
                "architecture.yaml": {
                    "generation": "SN0",
                    "architecture_ip": "IP27",
                    "kernel_ip": "IP27",
                },
                "assembly.yaml": {"board_ip": "IP29"},
            }
            for name, contract in contracts.items():
                layer_path = root / name
                document = yaml.safe_load(layer_path.read_text())
                document["layer"]["contract"] = contract
                layer_path.write_text(yaml.safe_dump(document, sort_keys=False))
            product_path = root / "product.yaml"
            product = yaml.safe_load(product_path.read_text())
            machine = product["layer"]["data"]["machine"]
            for field in (
                    "generation", "board_ip", "architecture_ip", "kernel_ip"):
                machine.pop(field)
            product_path.write_text(yaml.safe_dump(product, sort_keys=False))

            model = self.resolver.load_system_model(path)

        self.assertEqual(
            {field: model["machine"].get(field) for field in (
                "generation", "board_ip", "architecture_ip", "kernel_ip"
            )},
            {
                "generation": "SN0",
                "board_ip": "IP29",
                "architecture_ip": "IP27",
                "kernel_ip": "IP27",
            },
        )

    def test_layer_contract_conflict_names_machine_path(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.address_composition(directory)
            architecture_path = pathlib.Path(directory) / "architecture.yaml"
            architecture = yaml.safe_load(architecture_path.read_text())
            architecture["layer"]["contract"] = {"generation": "SN1"}
            architecture_path.write_text(
                yaml.safe_dump(architecture, sort_keys=False)
            )

            with self.assertRaisesRegex(
                    self.resolver.ResolutionError,
                    r"conflicting resolved field 'machine\.generation'"):
                self.resolver.load_system_model(path)

    def test_nested_layer_data_merge_and_conflict(self):
        for field, value, error in (
                ("lifecycle", "planned", None),
                ("display_description", "conflicting description",
                 r"conflicting resolved field 'machine\.emulation\.display_description'")):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as directory:
                path = self.address_composition(directory, role="emulation-target")
                architecture_path = pathlib.Path(directory) / "architecture.yaml"
                architecture = yaml.safe_load(architecture_path.read_text())
                architecture["layer"]["data"]["machine"] = {"emulation": {field: value}}
                architecture_path.write_text(yaml.safe_dump(architecture))
                if error:
                    with self.assertRaisesRegex(self.resolver.ResolutionError, error):
                        self.resolver.load_system_model(path)
                else:
                    resolved = self.resolver.load_system_model(path)
                    self.assertEqual(resolved["machine"]["emulation"], {
                        "machine_name": "address-projection-fixture",
                        "display_description": "Address projection fixture",
                        "profile": "base", "lifecycle": "planned",
                    })

    def test_resolved_origin200_profiles_omit_machine_node_count(self):
        for name in ("origin200", "origin200-dual"):
            with self.subTest(name=name):
                model = self.real_model(name)

                self.assertNotIn("nodes", model["machine"])

    def test_origin200_profiles_do_not_author_implementation_inventory(self):
        for name in ("origin200", "origin200-dual"):
            with self.subTest(name=name):
                product = yaml.safe_load((
                    INPUTS / "layers" / "products" /
                    f"{name}.yaml"
                ).read_text())
                emulation = product["layer"]["data"]["machine"]["emulation"]

                self.assertNotIn("source", emulation)
                self.assertNotIn("device_types", emulation)

    def test_origin200_boot_flash_has_hardware_contract_metadata(self):
        for name, component_id in (
                ("origin200", "boot-flash"),
                ("origin200-dual", "node0-boot-flash")):
            with self.subTest(name=name):
                model = self.real_model(name)
                flash = next(
                    component for component in model["components"]
                    if component["id"] == component_id
                )

                self.assertEqual(
                    flash["datasheet_contracts"],
                    ["qemu:docs/system/mips/sgi/prom-image.rst"],
                )

    def test_reference_module_composition_remains_graphless(self):
        model = self.real_model("origin2000")

        self.assertEqual(model["machine"]["model_role"], "reference")
        self.assertNotIn("config_graph", model)
        self.assertNotIn("config_graphs", model)
        self.assertNotIn(
            "config_graph_instances", model["composition"]
        )

    def test_dual_projection_keeps_optional_components_on_their_node(self):
        model = self.real_model("origin200-dual")

        optional = [
            component for component in model["components"]
            if component.get("optional") is not None
        ]
        self.assertEqual(len(optional), 1)
        self.assertEqual(optional[0]["id"], "node0-junk-uart")
        self.assertEqual(optional[0]["parent"], "node0-hub")
        self.assertEqual(
            optional[0]["optional"],
            {"node": 0, "default_enabled": False},
        )
        self.assertNotIn(
            "node1-junk-uart",
            {component["id"] for component in model["components"]},
        )

    def test_resolved_components_do_not_repeat_graph_membership(self):
        model = self.real_model("origin200")

        self.assertTrue(all("klconfig" not in component
                            for component in model["components"]))

    def test_dual_projection_preserves_cpu_hardware_kind(self):
        model = self.real_model("origin200-dual")

        cpus = {
            component["id"]: component
            for component in model["components"]
            if component.get("kind") == "cpu"
        }
        self.assertEqual(
            {component_id: component["hardware_kind"]
             for component_id, component in cpus.items()},
            {
                "node0-cpu0": "r10000",
                "node0-cpu1": "r10000",
                "node1-cpu0": "r10000",
                "node1-cpu1": "r10000",
            },
        )

    def test_dual_projection_preserves_module_roles(self):
        model = self.real_model("origin200-dual")

        self.assertEqual(
            [entry.get("module_role")
             for entry in model["composition"]["module_instances"]],
            ["MASTER", "SLAVE"],
        )

    def test_config_graph_derivation_rejects_a_module_without_a_role(self):
        with self.assertRaisesRegex(
                self.resolver.ResolutionError, r"module instance 0 requires module_role"):
            self.load_layer_changes([
                ("origin200-dual.yaml",
                 lambda document: document["module_instances"][0].pop("module_role")),
            ], name="origin200-dual")

    def test_module_graphs_require_matching_population(self):
        # memory_bank_mib_per_node is derived from memory_kits_per_node, always
        # one kit per node, so a length mismatch there is no longer authorable;
        # only a module_instances count mismatch reaches this check now.
        with self.assertRaisesRegex(
                self.resolver.ResolutionError, "must describe the same nodes"):
            self.load_layer_changes([
                ("origin200-dual.yaml", lambda doc: doc["module_instances"].pop()),
            ], name="origin200-dual")

    def test_node_free_brick_does_not_count_as_a_processor_node(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = self.module_composition(root / "brick", {
                "components": [{"id": "brick", "name": "Graphics brick",
                                "kind": "chassis"}],
            })
            resolved = {
                "components": [
                    {"id": "system", "name": "System", "kind": "chassis"},
                    {"id": "node", "name": "Existing processor node",
                     "kind": "node-asic", "parent": "system"},
                ],
                "buses": [], "connections": [], "interrupts": [],
                "open_questions": [],
            }
            instances = self.resolver._merge_module_instances(
                root / "system.yaml", {
                    "module_instances": [{
                        "model": source.relative_to(root).as_posix(),
                        "prefix": "graphics-", "parent": "system",
                    }],
                }, resolved, {"product": {"kind": "product", "id": "test"},
                             "assembly": {"kind": "assembly", "id": "ip29"}}, {
                    "nodes": 1, "cpus_per_node": [2],
                    "memory_bank_mib_per_node": [[512]],
                })

        self.assertEqual(len(instances), 1)
        self.assertIn("graphics-brick",
                      {item["id"] for item in resolved["components"]})


    @staticmethod
    def _fit_menet_at_io3(document):
        document["module_instances"].append({
            "model": "layers/modules/menet.yaml", "prefix": "io3-",
            "parent": "xio-position-io3",
            "bus_bindings": {"upstream": "crossbow-link-c"},
        })

    def test_a_board_fitted_in_a_connector_is_not_a_node_module(self):
        # A MENET in a GIGAchannel XIO position is a module instance too,
        # but the one-node population counts only the attached module.
        model = self.load_layer_changes([
            ("layers/modules/menet.yaml",
             lambda doc: doc["layer"].pop("requires", None)),
            ("origin200-gigachannel.yaml", self._fit_menet_at_io3),
        ], name="origin200-gigachannel")
        self.assertEqual(
            [entry.get("module_role")
             for entry in model["composition"]["module_instances"]],
            ["MASTER", None],
        )
        self.assertIn("io3-menet",
                      {item["id"] for item in model["components"]})

    def test_a_node_module_beside_a_fitted_board_still_needs_a_role(self):
        def drop_role_and_fit(document):
            document["module_instances"][0].pop("module_role")
            self._fit_menet_at_io3(document)

        with self.assertRaisesRegex(
                self.resolver.ResolutionError,
                r"module instance 0 requires module_role"):
            self.load_layer_changes([
                ("layers/modules/menet.yaml",
                 lambda doc: doc["layer"].pop("requires", None)),
                ("origin200-gigachannel.yaml", drop_role_and_fit),
            ], name="origin200-gigachannel")

    def test_reference_rejects_authored_implementation_state(self):
        cases = (
            ("architecture layer address space",
             {"architecture_emulation": True},
             r"reference model cannot contain implementation state at "
             r"address_spaces"),
            ("product layer component", {"product_emulation": True},
             r"reference model cannot contain implementation state at "
             r"components"),
            ("component identity field",
             {"product_data": {"components": [{"id": "hub", "identity": {
                 "notes": "hardware identity",
                 "implementation_status": "implemented"}}]}},
             r"reference model cannot contain implementation state at "
             r"components\[0\]\.identity\.implementation_status"),
            ("component modelled state",
             {"product_data": {"components": [
                 {"id": "hub", "modelled": "full"}]}},
             r"reference model cannot contain implementation state at "
             r"components\[0\]\.modelled"),
            ("interrupt implementation state",
             {"product_data": {"interrupts": [
                 {"id": "hardware-irq",
                  "implementation_status": "implemented"}]}},
             r"reference model cannot contain implementation state at "
             r"interrupts\[0\]\.implementation_status"),
        )
        for name, options, message in cases:
            with self.subTest(name=name), \
                    tempfile.TemporaryDirectory() as directory:
                path = self.address_composition(directory, **options)
                with self.assertRaisesRegex(
                        self.resolver.ResolutionError, message):
                    self.resolver.load_system_model(path)

    def test_module_instance_rejects_invalid_references(self):
        source_model = {
            "components": [
                {"id": "chassis", "name": "Chassis", "kind": "chassis"},
                {"id": "board", "name": "Board", "kind": "board",
                 "parent": "chassis"},
            ],
        }
        cases = (
            ("model does not exist", lambda path: {"model": "missing.yaml"},
             r"missing module-instance model"),
            ("unsupported connection field deletion",
             lambda path: {"omit_connection_fields": []},
             r"unsupported field 'omit_connection_fields'"),
            ("model outside the composition directory",
             lambda path: {"model": "../outside.yaml"},
             r"outside its composition directory"),
            ("model is the composition itself",
             lambda path: {"model": path.name},
             r"recursive module-instance composition"),
            ("enclosing parent does not exist",
             lambda path: {"parent": "missing-system"},
             r"names unknown enclosing parent 'missing-system'"),
        )
        for name, override, message in cases:
            with self.subTest(name=name), \
                    tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                path = self.address_composition(root)
                source = self.module_composition(root / "module", source_model)
                parent = self.resolver.load_system_model(
                    path)["components"][0]["id"]
                composition = yaml.safe_load(path.read_text())
                composition["module_instances"] = [{
                    "model": source.relative_to(root).as_posix(),
                    "prefix": "node0-",
                    "parent": parent,
                    **override(path),
                }]
                path.write_text(yaml.safe_dump(composition, sort_keys=False))

                with self.assertRaisesRegex(
                        self.resolver.ResolutionError, message):
                    self.resolver.load_system_model(path)

    def test_module_instance_rejects_duplicate_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = self.address_composition(root)
            enclosing_parent = self.resolver.load_system_model(path)["components"][0]["id"]
            source_model = {
                "components": [
                    {"id": "chassis", "name": "Chassis", "kind": "chassis"},
                    {"id": "board", "name": "Board", "kind": "board",
                     "parent": "chassis"},
                ],
            }
            source = self.module_composition(root / "module", source_model)
            composition = yaml.safe_load(path.read_text())
            instance = {
                "model": source.relative_to(root).as_posix(),
                "prefix": "node0-",
                "parent": enclosing_parent,
            }
            composition["module_instances"] = [instance, dict(instance)]
            path.write_text(yaml.safe_dump(composition, sort_keys=False))

            with self.assertRaisesRegex(
                    self.resolver.ResolutionError,
                    r"reuses prefix 'node0-'"):
                self.resolver.load_system_model(path)

    def test_module_instance_rejects_prefixed_id_collision(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            path = self.address_composition(root)
            enclosing_parent = self.resolver.load_system_model(path)["components"][0]["id"]
            source_model = {
                "components": [
                    {"id": "chassis", "name": "Chassis", "kind": "chassis"},
                    {"id": "board", "name": "Board", "kind": "board",
                     "parent": "chassis"},
                ],
            }
            source = self.module_composition(root / "module", source_model)
            source_id = source_model["components"][0]["id"]

            composition = yaml.safe_load(path.read_text())
            composition["module_instances"] = [{
                "model": source.relative_to(root).as_posix(),
                "prefix": "node0-",
                "parent": enclosing_parent,
            }]
            path.write_text(yaml.safe_dump(composition, sort_keys=False))

            for reference in composition["layers"]:
                layer_path = pathlib.Path(directory) / reference
                layer = yaml.safe_load(layer_path.read_text())
                data = layer["layer"].setdefault("data", {})
                if "components" not in data:
                    continue
                data["components"].append({
                    "id": "node0-" + source_id,
                    "name": "Collision",
                    "kind": "board",
                })
                layer_path.write_text(yaml.safe_dump(layer, sort_keys=False))
                break
            else:
                self.fail("fixture composition has no component-owning layer")

            with self.assertRaisesRegex(
                    self.resolver.ResolutionError,
                    r"collides with component id"):
                self.resolver.load_system_model(path)

    def write_layer_with_source_table(self, directory, sources, component_sources):
        path = pathlib.Path(directory) / "layer.yaml"
        path.write_text(yaml.safe_dump({
            "sources": sources,
            "layer_version": 1,
            "layer": {
                "kind": "module",
                "id": "fixture-module",
                "requires": {"assembly": "ip29"},
                "data": {"components": [{
                    "id": "chassis", "name": "Chassis", "kind": "chassis",
                    "provenance": "vendor-document", "sources": component_sources,
                }]},
            },
        }, sort_keys=False))
        return path

    def test_source_table_key_expands_to_its_citation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_layer_with_source_table(
                directory,
                {"guide": "Origin200 Owner's Guide 007-3708-002 revision 002"},
                ["guide"],
            )
            layer = self.resolver._layer(path)
        self.assertEqual(
            layer["data"]["components"][0]["sources"],
            ["Origin200 Owner's Guide 007-3708-002 revision 002"],
        )

    def test_source_table_entry_must_be_a_non_empty_string(self):
        """The retired {prefix, suffix} split-entry form is gone: the citation
        checker (tools/check-citations) now resolves a fact's own sources:
        entries directly against the document catalogue, so a table entry is
        just the finished citation string, nothing more."""
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_layer_with_source_table(
                directory, {"bad": {"figure": "1-4"}}, ["bad"],
            )
            with self.assertRaisesRegex(
                    self.resolver.ResolutionError,
                    r"sources table entry 'bad' must be a non-empty string"):
                self.resolver._layer(path)

    def test_source_table_entry_rejects_an_empty_string(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_layer_with_source_table(
                directory, {"cite": ""}, ["cite"],
            )
            with self.assertRaisesRegex(
                    self.resolver.ResolutionError,
                    r"sources table entry 'cite' must be a non-empty string"):
                self.resolver._layer(path)

    def test_source_not_found_in_the_table_passes_through_unchanged(self):
        """A citation written out directly, without a table entry, still
        works -- the table only shortcuts repetition, it is never required."""
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_layer_with_source_table(
                directory, {"guide": "Origin200 Owner's Guide"},
                ["a literal citation with no table entry"],
            )
            layer = self.resolver._layer(path)
        self.assertEqual(
            layer["data"]["components"][0]["sources"],
            ["a literal citation with no table entry"],
        )

    def test_missing_source_table_leaves_sources_lists_untouched(self):
        """A document with no top-level "sources:" table at all (the shape
        every fixture in this file that predates provenance compaction
        uses) is not walked or altered."""
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "layer.yaml"
            path.write_text(yaml.safe_dump({
                "layer_version": 1,
                "layer": {
                    "kind": "module", "id": "fixture-module",
                    "requires": {"assembly": "ip29"},
                    "data": {"components": [{
                        "id": "chassis", "name": "Chassis", "kind": "chassis",
                        "provenance": "vendor-document", "sources": ["a bare citation"],
                    }]},
                },
            }, sort_keys=False))
            layer = self.resolver._layer(path)
        self.assertEqual(layer["data"]["components"][0]["sources"], ["a bare citation"])

    def test_module_instance_rejects_invalid_containment_or_missing_capture_basis(self):
        for components, message in (
                ([{"id": "board-a", "parent": "board-b"},
                  {"id": "board-b", "parent": "board-a"}],
                 r"has no containment root"),
                ([{"id": "chassis-a"}, {"id": "board-a", "parent": "chassis-a"},
                  {"id": "chassis-b"}, {"id": "board-b", "parent": "chassis-b"}],
                 r"has multiple containment roots: chassis-a, chassis-b"),
                ([{"id": "chassis"}, {"id": "board", "parent": "chassis"},
                  {"id": "connector"}],
                 r"has multiple containment roots: chassis, connector"),
                ([{"id": "chassis", "provenance": "observed-hardware",
                   "sources": ["source module capture"]}],
                 r"copies observed-hardware claims without an instance basis")):
            with self.subTest(message=message):
                with self.assertRaisesRegex(self.resolver.ResolutionError, message):
                    self.load_module({"components": components})

    def test_module_instance_adopts_every_root_of_a_flat_module(self):
        # A unibody product's module layer (the Origin 300 base module)
        # authors every component at the top level; an instance of it that
        # opts in parents each root under the instance's parent, and leaves
        # the interior containment alone.
        model = self.load_module(
            {"components": [{"id": "chassis"}, {"id": "board", "parent": "chassis"},
                            {"id": "connector"}]},
            validate=False, adopt_roots=True)
        parents = {item["id"]: item.get("parent") for item in model["components"]
                   if item["id"].startswith("node0-")}
        self.assertEqual(parents, {"node0-chassis": "hub", "node0-board": "node0-chassis",
                                   "node0-connector": "hub"})
        with self.assertRaisesRegex(self.resolver.ResolutionError,
                                    r"adopt_roots must be true or false"):
            self.load_module({"components": [{"id": "chassis"}]},
                             validate=False, adopt_roots="yes")



class ConfigurationPeerLinkTests(unittest.TestCase):
    def test_configuration_links_require_a_list(self):
        path = INPUTS / "origin300-two-chassis.yaml"
        composition = yaml.safe_load(path.read_text())
        composition["links"] = None
        with self.assertRaisesRegex(
                _resolver.ResolutionError, "must be a list"):
            _resolver.resolve_system_model(path, composition)

    def test_configuration_links_use_expanded_component_ids(self):
        path = INPUTS / "origin300-two-chassis.yaml"
        composition = yaml.safe_load(path.read_text())
        links = [{"kind": "l1-peer", "source": "master-l1",
                 "target": "slave-l1"},
                 {"kind": "l1-peer", "source": "slave-l1",
                 "target": "master-l1"}]
        composition["links"] = links
        resolved = _resolver.resolve_system_model(path, composition)
        self.assertEqual(resolved["links"], links)
        links[0]["source"] = "changed"
        self.assertEqual(resolved["links"][0]["source"], "master-l1")


class Origin3000BrickTests(unittest.TestCase):
    """The Origin 3000 brick layers and how a composition joins them."""

    ORIGIN3200 = INPUTS / "origin3200.yaml"

    def resolve(self, path=None):
        return _resolver.load_system_model(path or self.ORIGIN3200)

    def test_cbrick_has_one_xio_ii_connector(self):
        model = self.resolve()
        components = {item["id"]: item for item in model["components"]}
        self.assertNotIn("c0-host-port-a", components)
        self.assertNotIn("c0-host-port-b", components)
        connector = components["c0-xio-ii-connector"]
        self.assertEqual(connector["name"], "XIO (II) connector")
        self.assertEqual(connector["connector"]["interface"], "Crosstown2")
        bus = next(item for item in model["buses"]
                   if item["id"] == "c0-xio-ii")
        self.assertEqual((bus["kind"], bus["master"]),
                         ("xio", "c0-bedrock"))
        self.assertIn(
            {"component": "c0-xio-ii-connector", "bus": "c0-xio-ii",
             "role": "endpoint"},
            [{field: item[field] for field in ("component", "bus", "role")}
             for item in model["connections"]])

    def test_attached_ibrick_reaches_the_cbrick_through_bus_bindings(self):
        model = self.resolve()
        attachments = {
            (item["component"], item["bus"], item["role"])
            for item in model["connections"]}
        self.assertIn(("ibrick-xbridge-crossbar", "c0-xio-ii",
                       "bridge-upstream"),
                      attachments)
        # The I-brick declares no bus of its own mastered by a component
        # outside it.
        for bus in model["buses"]:
            if bus["id"].startswith("ibrick-"):
                self.assertTrue(bus["master"].startswith("ibrick-"),
                                bus["id"])

    def test_rbrick_layer_names_only_its_own_enclosure(self):
        # Instanced by itself, the layer binds nothing: each of its eight
        # ports has its own cable bus that a composition attaches to.
        layer = yaml.safe_load(
            (INPUTS / "layers/modules/r-brick.yaml").read_text())
        graph = _resolver._instantiate_module_graph(
            copy.deepcopy(layer["layer"]["data"]), prefix="rbrick-",
            parent="rack", omit_buses=[])
        self.assertEqual(
            sorted((item["component"], item["bus"], item["fabric_port"])
                   for item in graph["connections"]
                   if item["component"] == "rbrick-router"),
            sorted(("rbrick-router", f"rbrick-port-{port}-cable", port)
                   for port in range(1, 9)))
        for bus in graph["buses"]:
            self.assertEqual(bus["master"], "rbrick-router", bus["id"])
            self.assertEqual(bus["link"], {"kind": "cable"}, bus["id"])
        router = next(item for item in graph["components"]
                      if item["id"] == "rbrick-router")
        self.assertEqual(router["router_asic"]["node_ports"], [2, 3, 4, 5])
        brick = next(item for item in graph["components"]
                     if item["id"] == "rbrick-brick")
        self.assertNotIn("brick_type", brick)

    def test_module_naming_a_component_outside_its_instance_is_refused(self):
        outside = (
            ("a connection", {
                "components": [{"id": "brick", "name": "Brick",
                                "kind": "chassis"}],
                "buses": [{"id": "link", "name": "Link", "kind": "xio",
                           "master": "brick"}],
                "connections": [{"component": "bedrock", "bus": "link",
                                 "role": "master"}]}),
            ("a bus master", {
                "components": [{"id": "brick", "name": "Brick",
                                "kind": "chassis"}],
                "buses": [{"id": "link", "name": "Link", "kind": "xio",
                           "master": "bedrock"}],
                "connections": [{"component": "brick", "bus": "link",
                                 "role": "endpoint"}]}),
        )
        for name, contract in outside:
            with self.subTest(name=name), self.assertRaisesRegex(
                    _resolver.ResolutionError,
                    "names component 'bedrock' outside its instance"):
                _resolver._instantiate_module_graph(
                    contract, prefix="brick-", parent="chassis",
                    omit_buses=[])

    TWO_CROSSBARS = {
        "components": [
            {"id": "brick", "name": "Brick", "kind": "chassis"},
            {"id": "u0", "name": "U0", "kind": "crossbar", "parent": "brick",
             "crossbar": {"kind": "xbridge"}},
            {"id": "u1", "name": "U1", "kind": "crossbar", "parent": "brick",
             "crossbar": {"kind": "xbridge"}},
            *({"id": f"{chip}-port-{port:x}", "name": f"{chip} port {port:x}",
               "kind": "connector", "parent": chip,
               "connector": {"interface": "Crosstalk", "occupancy": "opaque",
                             "port": port}}
              for chip, port in (("u0", 9), ("u0", 8), ("u1", 8), ("u1", 0xd))),
        ],
        "buses": [
            {"id": "link-9", "name": "U0 9 to U1 8", "kind": "xio",
             "master": "u0"},
            {"id": "link-8", "name": "U0 8 to U1 D", "kind": "xio",
             "master": "u0"},
        ],
        "connections": [
            {"component": "u0-port-9", "bus": "link-9", "role": "endpoint"},
            {"component": "u1-port-8", "bus": "link-9", "role": "endpoint"},
            {"component": "u0-port-8", "bus": "link-8", "role": "endpoint"},
            {"component": "u1-port-d", "bus": "link-8", "role": "endpoint"},
        ],
        "xio_cables": [
            {"id": "first", "ends": ["u0-port-9", "u1-port-8"]},
            {"id": "second", "ends": ["u0-port-8", "u1-port-d"]},
        ],
    }

    def test_module_xio_cables_are_prefixed_with_their_instance(self):
        graph = _resolver._instantiate_module_graph(
            copy.deepcopy(self.TWO_CROSSBARS), prefix="p-", parent="rack",
            omit_buses=[])
        self.assertEqual(
            [(item["id"], item["ends"]) for item in graph["xio_cables"]],
            [("p-first", ["p-u0-port-9", "p-u1-port-8"]),
             ("p-second", ["p-u0-port-8", "p-u1-port-d"])])

    def test_two_cables_between_two_chips_on_distinct_ports_resolve(self):
        graph = _resolver._instantiate_module_graph(
            copy.deepcopy(self.TWO_CROSSBARS), prefix="p-", parent="rack",
            omit_buses=[])
        model = {"components": graph["components"], "buses": graph["buses"],
                 "connections": graph["connections"],
                 "xio_cables": graph["xio_cables"]}
        _resolver.model_composition.validate_xio_cables(model)

    def test_module_xio_cable_naming_an_outside_connector_is_refused(self):
        contract = copy.deepcopy(self.TWO_CROSSBARS)
        contract["xio_cables"][0]["ends"][1] = "c0-xio-ii-connector"
        with self.assertRaisesRegex(
                _resolver.ResolutionError,
                "names component 'c0-xio-ii-connector' outside"):
            _resolver._instantiate_module_graph(
                contract, prefix="p-", parent="rack", omit_buses=[])

    def check_rules(self, rules, bricks):
        _resolver.model_composition.validate_composition_rules(
            "fixture", rules, bricks)

    def test_composition_rules_count_each_brick_group(self):
        rules = {"counts": [
            {"bricks": ["c-brick"], "min": 1, "max": 2},
            {"bricks": ["r-brick"], "max": 0},
            {"bricks": ["i-brick", "p-brick", "x-brick"], "max": 2,
             "require": "i-brick"},
        ]}
        self.check_rules(rules, ["c-brick", "c-brick", "i-brick", "p-brick"])
        for name, bricks, message in (
                ("three C-bricks", ["c-brick"] * 3 + ["i-brick"],
                 "allows at most 2 of c-brick, got 3"),
                ("no C-brick", ["i-brick"],
                 "requires at least 1 of c-brick, got 0"),
                ("an R-brick", ["c-brick", "r-brick", "i-brick"],
                 "allows at most 0 of r-brick, got 1"),
                ("three I/O bricks",
                 ["c-brick", "i-brick", "p-brick", "x-brick"],
                 "allows at most 2 of i-brick, p-brick, x-brick, got 3"),
                ("no I-brick", ["c-brick", "p-brick"],
                 "requires at least one i-brick among its i-brick, p-brick, "
                 "x-brick"),
        ):
            with self.subTest(name=name), self.assertRaisesRegex(
                    _resolver.ResolutionError, message):
                self.check_rules(rules, bricks)

    def test_composition_rules_refuse_exclusive_and_unaccompanied_bricks(self):
        rules = {"exclusive": [["g-brick", "v-brick"]],
                 "requires": [{"brick": "n-brick", "with": "g-brick"}]}
        self.check_rules(rules, ["c-brick", "i-brick", "g-brick", "n-brick"])
        self.check_rules(rules, ["c-brick", "v-brick"])
        for name, bricks, message in (
                ("a G-brick and a V-brick", ["c-brick", "g-brick", "v-brick"],
                 "does not allow g-brick and v-brick together"),
                ("an N-brick without a G-brick", ["c-brick", "n-brick"],
                 "allows n-brick only with g-brick"),
        ):
            with self.subTest(name=name), self.assertRaisesRegex(
                    _resolver.ResolutionError, message):
                self.check_rules(rules, bricks)

    def test_composition_rules_must_be_well_formed(self):
        for rules, message in (
                ([], "must be a mapping"),
                ({"limits": []}, "unsupported field 'limits'"),
                ({"counts": [{"bricks": []}]}, "names no bricks"),
                ({"counts": [{"bricks": ["c-brick"], "min": 3, "max": 2}]},
                 "min 3 exceeds max 2"),
                ({"counts": [{"bricks": ["c-brick"], "require": "i-brick"}]},
                 "requires 'i-brick', which it does not count"),
                ({"exclusive": [["g-brick"]]}, "names fewer than two bricks"),
                ({"requires": [{"brick": "n-brick"}]}, "names brick and with"),
        ):
            with self.subTest(rules=rules), self.assertRaisesRegex(
                    _resolver.ResolutionError, message):
                self.check_rules(rules, ["c-brick"])

    def test_product_composition_rules_count_module_layers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            shutil.copytree(INPUTS, root, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns("*.json"))
            shutil.copy(INPUTS / "system-model.schema.json", root)
            product = root / "layers/products/origin3200.yaml"
            document = yaml.safe_load(product.read_text())
            document["layer"]["composition_rules"] = {
                "counts": [{"bricks": ["i-brick"], "max": 0}]}
            product.write_text(yaml.safe_dump(document, sort_keys=False))
            with self.assertRaisesRegex(
                    _resolver.ResolutionError,
                    "product 'origin3200' allows at most 0 of i-brick, "
                    "got 1"):
                _resolver.load_system_model(root / "origin3200.yaml")


    def test_origin3200_resolves_two_cbricks_and_an_ibrick(self):
        model = self.resolve()
        components = {item["id"]: item for item in model["components"]}
        self.assertEqual(
            sorted(cid for cid, item in components.items()
                   if item["kind"] == "node-asic"),
            ["c0-bedrock", "c1-bedrock"])
        self.assertEqual(components["c0-chassis"]["brick_type"], "C")
        # Each brick is a module of its own under the rack, so the
        # compiler gives each one its own module record.
        self.assertEqual(
            sorted(cid for cid, item in components.items()
                   if item["kind"] == "module"),
            ["c0-chassis", "c1-chassis", "ibrick-brick"])
        self.assertTrue(all(components[cid]["parent"] == "rack"
                            for cid in ("c0-chassis", "c1-chassis",
                                        "ibrick-brick")))
        self.assertEqual(
            [(item["id"], item["ends"]) for item in model["xio_cables"]],
            [("c0-to-ibrick", ["c0-xio-ii-connector", "ibrick-port-a"])])
        attachments = {(item["component"], item["bus"], item["role"])
                       for item in model["connections"]}
        self.assertIn(("ibrick-xbridge-crossbar", "c0-xio-ii",
                       "bridge-upstream"), attachments)
        self.assertTrue({("c0-bedrock", "ni-cable", "master"),
                         ("c1-bedrock", "ni-cable", "endpoint")}
                        <= attachments)

    def resolve_origin3200_with(self, instances):
        composition = yaml.safe_load(self.ORIGIN3200.read_text())
        composition["module_instances"] = instances(
            composition["module_instances"])
        return _resolver.resolve_system_model(self.ORIGIN3200, composition)

    def test_origin3200_refuses_brick_sets_outside_its_configuration(self):
        def third_cbrick(instances):
            return instances + [dict(instances[1], prefix="c2-")]

        def without_ibrick(instances):
            return instances[:2]

        def three_ibricks(instances):
            extra = [dict(instances[2], prefix=prefix,
                          bus_bindings={"host-port-a-xio": "c1-xio-ii"})
                     for prefix in ("ib1-", "ib2-")]
            return instances + extra

        def cbrick_rbrick_ibrick(instances):
            # One C-brick, one R-brick and one I-brick.
            rbrick = {"model": "layers/modules/r-brick.yaml",
                      "prefix": "rbrick-", "parent": "rack"}
            return [instances[0], rbrick, instances[2]]

        for name, instances, message in (
                ("three C-bricks", third_cbrick,
                 "allows at most 2 of c-brick, got 3"),
                ("no I-brick", without_ibrick,
                 "requires at least one i-brick among"),
                ("three I/O bricks", three_ibricks,
                 "allows at most 2 of i-brick, p-brick, x-brick, got 3"),
                ("one C-brick, one R-brick and one I-brick",
                 cbrick_rbrick_ibrick, "allows at most 0 of r-brick, got 1"),
        ):
            with self.subTest(name=name), self.assertRaisesRegex(
                    _resolver.ResolutionError,
                    f"product 'origin3200' {message}"):
                self.resolve_origin3200_with(instances)

    def load_origin3200_with(self, changes):
        """Resolve the Origin 3200 from a copy whose layers @changes edit."""
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            shutil.copytree(INPUTS, root, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns("*.json"))
            shutil.copy(INPUTS / "system-model.schema.json", root)
            for relative, mutate in changes:
                path = root / relative
                document = yaml.safe_load(path.read_text())
                mutate(document)
                path.write_text(yaml.safe_dump(document, sort_keys=False))
            return _resolver.load_system_model(root / "origin3200.yaml")

    def test_cbrick_carries_two_two_processor_interfaces(self):
        model = self.resolve()
        components = {item["id"]: item for item in model["components"]}
        self.assertEqual(
            [(cid, components[cid]["processor_slice"])
             for cid in ("c0-cpu0", "c0-cpu1", "c0-cpu2", "c0-cpu3")],
            [("c0-cpu0", "A"), ("c0-cpu1", "B"), ("c0-cpu2", "C"),
             ("c0-cpu3", "D")])
        attached = {(item["component"], item["bus"], item["role"])
                    for item in model["connections"]
                    if item["bus"] in ("c0-sysad0", "c0-sysad1")}
        self.assertEqual(attached, {
            ("c0-cpu0", "c0-sysad0", "master"),
            ("c0-cpu1", "c0-sysad0", "master"),
            ("c0-cpu2", "c0-sysad1", "master"),
            ("c0-cpu3", "c0-sysad1", "master"),
            ("c0-bedrock", "c0-sysad0", "endpoint"),
            ("c0-bedrock", "c0-sysad1", "endpoint")})

    def test_origin3200_carries_the_cbrick_selected_fit(self):
        model = self.resolve()
        cpus = [item for item in model["components"] if item["kind"] == "cpu"]
        self.assertEqual(len(cpus), 8)
        for cpu in cpus:
            with self.subTest(cpu=cpu["id"]):
                self.assertEqual(cpu["hardware_kind"], "r12000")
                self.assertEqual(
                    {field: cpu["identity"][field] for field in (
                        "clock_mhz", "cache_mib", "cache_clock_mhz",
                        "processor_identification", "fpu_identification")},
                    {"clock_mhz": 400, "cache_mib": 8, "cache_clock_mhz": 266,
                     "processor_identification": "0x0e35",
                     "fpu_identification": "0x0935"})
        population = model["composition"]["population"]
        self.assertEqual(population["cpus_per_node"], [4, 4])
        self.assertEqual(population["dimms_per_node"], [8, 8])
        self.assertEqual(population["memory_mib_per_node"], [4096, 4096])
        self.assertEqual(population["memory_bank_mib_per_node"],
                         [[512] * 8, [512] * 8])
        self.assertEqual(model["machine"]["model_role"], "emulation-target")
        self.assertEqual(model["machine"]["emulation"]["lifecycle"],
                         "implemented")

    def test_origin3200_refuses_a_fit_outside_the_selected_fit(self):
        product = "layers/products/origin3200.yaml"

        def default(clock_mhz, cache_mib):
            def change(document):
                document["layer"]["cpu_selection"]["default"] = {
                    "clock_mhz": clock_mhz, "cache_mib": cache_mib}
            return change

        def catalogue(part, clock_mhz, cache_mib):
            def change(document):
                entries = document["layer"]["data"]["processors"]
                entry = next(item for item in entries if item["part"] == part)
                entry["variants"].append(
                    {"clock_mhz": clock_mhz, "cache_mib": cache_mib})
            return change

        def cbrick_part(part):
            def change(document):
                for component in document["layer"]["data"]["components"]:
                    if component["kind"] == "cpu":
                        component["hardware_kind"] = part
            return change

        def two_processors(document):
            document["layer"]["population"]["cpus_per_node"] = [2, 2]

        def half_memory(document):
            document["layer"]["memory_kits_per_node"] = [
                "kit-2048-four-512-banks"] * 2

        def four_bank_kit(document):
            document["layer"]["memory_kits"].append(
                {"id": "kit-2048-four-512-banks", "dimms": 4, "dimm_mib": 512,
                 "banks": [512] * 4, "provenance": "derived",
                 "sources": ["Synthetic resolver fixture."]})

        sn1 = "layers/architectures/sn1.yaml"
        ip35 = "layers/assemblies/ip35.yaml"
        cbrick = "layers/modules/c-brick.yaml"
        for name, changes, message in (
                ("a 360 MHz, 4 MiB R12000",
                 [(sn1, catalogue("r12000", 360, 4)),
                  (product, default(360, 4))],
                 "does not support CPU 'c0-cpu0', r12000 at 360 MHz with "
                 "4 MiB of cache"),
                ("the 500 MHz R14000 PIMM option",
                 [(sn1, catalogue("r14000", 500, 8)),
                  (cbrick, cbrick_part("r14000")),
                  (product, default(500, 8))],
                 "does not support CPU 'c0-cpu0', r14000 at 500 MHz with "
                 "8 MiB of cache"),
                ("two processors per C-brick", [(product, two_processors)],
                 r"selected CPU population \[2, 2\] is not supported"),
                ("2 GiB per C-brick",
                 [(ip35, four_bank_kit), (product, half_memory)],
                 r"product 'origin3200' does not support node 0 memory banks "
                 r"\[512, 512, 512, 512, 0, 0, 0, 0\]"),
        ):
            with self.subTest(name=name), self.assertRaisesRegex(
                    _resolver.ResolutionError, message):
                self.load_origin3200_with(changes)

    def test_ip35_banks_on_one_dimm_pair_are_fitted_alike(self):
        ip35 = "layers/assemblies/ip35.yaml"
        product = "layers/products/origin3200.yaml"

        def kit_banks(banks):
            def change(document):
                kit = document["layer"]["memory_kits"][0]
                kit.update(banks=banks, dimms=len(banks))
            return change

        def any_rows(document):
            del document["layer"]["topology"]["supported_bank_mib_per_node"]

        for banks, message in (
                ([1024, 512] + [512] * 6,
                 "node 0 banks 0 to 1 share DIMM pair 0 and must be fitted "
                 "alike"),
                ([512] * 7,
                 "node 0 banks 6 to 7 share DIMM pair 3 and must be fitted "
                 "alike"),
        ):
            with self.subTest(banks=banks), self.assertRaisesRegex(
                    _resolver.ResolutionError, message):
                self.load_origin3200_with(
                    [(ip35, kit_banks(banks)), (product, any_rows)])
        # Pairs may differ from each other.
        mixed = [1024, 1024] + [512] * 6
        model = self.load_origin3200_with(
            [(ip35, kit_banks(mixed)), (product, any_rows)])
        self.assertEqual(
            model["composition"]["population"]["memory_bank_mib_per_node"],
            [mixed, mixed])

    def test_origin3200_compiles_as_an_implemented_target(self):
        from sgi_model.model_compile import compile_model
        entry = compile_model(self.resolve())
        self.assertEqual([node["bedrock"]["board_id_word"]
                          for node in entry["nodes"]], [2, 2])
        assembly = entry["nodes"][0]["cpu_slots"][0]["assembly"]
        self.assertEqual(
            (assembly["kind"], assembly["prid"], assembly["fpu_prid"],
             assembly["mode_bits"]),
            ("R12000", 0x0e35, 0x0935, 0x0014a785))
        self.assertEqual(entry["ip35_firmware"],
                         {"machine_type": 0, "cpu_mode_word": 0x5414a785})
        self.assertEqual(
            [item.get("l1", {}).get("profile")
             for item in entry["controllers"]],
            ["cbrick", "cbrick", "ibrick"])
        self.assertEqual(entry["l1_peer_links"], [
            {"source_controller_index": 0, "target_controller_index": 1},
            {"source_controller_index": 1, "target_controller_index": 0}])
        self.assertEqual([module["initial_module_number"]
                          for module in entry["modules"]], [4, 7, 14])

    def test_origin3200_refuses_two_cbricks_with_one_module_number(self):
        def repeat(document):
            numbers = document["layer"]["data"]["machine"]["emulation"][
                "initial_module_numbers"]
            numbers[1]["number"] = numbers[0]["number"]

        with self.assertRaisesRegex(
                _resolver.ResolutionError,
                "initial module identity has repeated number 4"):
            self.load_origin3200_with(
                [("layers/products/origin3200.yaml", repeat)])

    def test_origin3200_refuses_two_bedrocks_with_one_serial(self):
        def repeat(document):
            identities = document["layer"]["data"]["machine"]["emulation"][
                "synthetic_identities"]
            identities[1]["serial"] = identities[0]["serial"]

        with self.assertRaisesRegex(
                _resolver.ResolutionError,
                "synthetic serial 0x51454d554e00 is given to both "
                "'c0-bedrock' and 'c1-bedrock'"):
            self.load_origin3200_with(
                [("layers/products/origin3200.yaml", repeat)])

    def test_unrouted_two_chassis_refuses_one_serial_twice(self):
        # Before this check a repeated serial passed resolution and
        # compilation on the direct-cabled Origin 300 pair.
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            shutil.copytree(INPUTS, root, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns("*.json"))
            shutil.copy(INPUTS / "system-model.schema.json", root)
            path = root / "layers/products/origin300-two-chassis.yaml"
            document = yaml.safe_load(path.read_text())
            identities = document["layer"]["data"]["machine"]["emulation"][
                "synthetic_identities"]
            slave = next(item for item in identities
                         if item["component"] == "slave-bedrock")
            slave["serial"] = "0x51454d554e00"
            path.write_text(yaml.safe_dump(document, sort_keys=False))
            with self.assertRaisesRegex(
                    _resolver.ResolutionError,
                    "synthetic serial 0x51454d554e00 is given to both "
                    "'master-bedrock' and 'slave-bedrock'"):
                _resolver.load_system_model(
                    root / "origin300-two-chassis.yaml")

    def test_composition_may_keep_the_product_as_a_reference(self):
        for path in (INPUTS / "origin3200-xbrick.yaml",
                     INPUTS / "origin3200-pbrick.yaml"):
            with self.subTest(path=path.name):
                machine = self.resolve(path)["machine"]
                self.assertEqual(machine["model_role"], "reference")
                self.assertNotIn("emulation", machine)
                self.assertNotIn("harness", machine)

    def test_composition_sets_only_the_reference_role(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            shutil.copytree(INPUTS, root, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns("*.json"))
            shutil.copy(INPUTS / "system-model.schema.json", root)
            path = root / "origin3200-xbrick.yaml"
            path.write_text(path.read_text().replace(
                "model_role: reference", "model_role: emulation-target"))
            with self.assertRaisesRegex(
                    _resolver.ResolutionError,
                    "may set model_role reference only"):
                _resolver.load_system_model(path)

    ORIGIN3200_XBRICK = INPUTS / "origin3200-xbrick.yaml"

    def test_xbrick_layer_puts_four_slots_on_crosstalk_ports(self):
        model = self.resolve(self.ORIGIN3200_XBRICK)
        components = {item["id"]: item for item in model["components"]}
        ports = {cid: item["connector"]["port"]
                 for cid, item in components.items()
                 if cid.startswith("xbrick-") and item["kind"] == "connector"}
        self.assertEqual(ports, {
            "xbrick-port-a": 0xa, "xbrick-port-b": 0xb,
            "xbrick-slot-1": 0x8, "xbrick-slot-2": 0x9,
            "xbrick-slot-3": 0xc, "xbrick-slot-4": 0xd,
        })
        for cid in ports:
            self.assertEqual(components[cid]["parent"],
                             "xbrick-xbridge-crossbar", cid)
        # Each slot is the far end of its own crossbar-mastered XIO bus,
        # and the brick declares no PCI bus.
        buses = {item["id"]: item for item in model["buses"]
                 if item["id"].startswith("xbrick-")}
        self.assertEqual(
            sorted(bid for bid, item in buses.items()
                   if item["master"] == "xbrick-xbridge-crossbar"),
            ["xbrick-port-b-xio", "xbrick-slot-1-xio", "xbrick-slot-2-xio",
             "xbrick-slot-3-xio", "xbrick-slot-4-xio"])
        self.assertFalse([bid for bid, item in buses.items()
                          if item["kind"] == "pci"])
        self.assertNotIn("brick_type", components["xbrick-brick"])

    def test_origin3200_xbrick_hangs_the_xbrick_off_the_second_cbrick(self):
        model = self.resolve(self.ORIGIN3200_XBRICK)
        self.assertEqual(
            [(item["id"], item["ends"]) for item in model["xio_cables"]],
            [("c0-to-ibrick", ["c0-xio-ii-connector", "ibrick-port-a"]),
             ("c1-to-xbrick", ["c1-xio-ii-connector", "xbrick-port-a"])])
        attachments = {(item["component"], item["bus"], item["role"])
                       for item in model["connections"]}
        self.assertIn(("ibrick-xbridge-crossbar", "c0-xio-ii",
                       "bridge-upstream"), attachments)
        self.assertIn(("xbrick-xbridge-crossbar", "c1-xio-ii",
                       "bridge-upstream"), attachments)
        self.assertEqual(
            sorted(item["id"] for item in model["components"]
                   if item["kind"] == "module"),
            ["c0-chassis", "c1-chassis", "ibrick-brick", "xbrick-brick"])

    def test_origin3200_xbrick_counts_against_the_io_brick_limit(self):
        composition = yaml.safe_load(self.ORIGIN3200_XBRICK.read_text())
        composition["module_instances"].append(dict(
            composition["module_instances"][3], prefix="xb2-",
            bus_bindings={"host-port-a-xio": "c0-xio-ii"}))
        with self.assertRaisesRegex(
                _resolver.ResolutionError,
                "product 'origin3200' allows at most 2 of i-brick, p-brick, "
                "x-brick, got 3"):
            _resolver.resolve_system_model(self.ORIGIN3200_XBRICK,
                                           composition)

    def test_composition_xio_cables_must_be_a_list(self):
        composition = yaml.safe_load(self.ORIGIN3200_XBRICK.read_text())
        composition["xio_cables"] = {"id": "c1-to-xbrick"}
        with self.assertRaisesRegex(
                _resolver.ResolutionError,
                "configuration xio_cables must be a list"):
            _resolver.resolve_system_model(self.ORIGIN3200_XBRICK,
                                           composition)

    ORIGIN3200_PBRICK = INPUTS / "origin3200-pbrick.yaml"

    def test_pbrick_layer_carries_the_figure_2_24_links(self):
        model = self.resolve(self.ORIGIN3200_PBRICK)
        components = {item["id"]: item for item in model["components"]}
        self.assertEqual(
            sorted(cid for cid, item in components.items()
                   if item["kind"] == "crossbar" and cid.startswith("pbrick-")),
            ["pbrick-u0-crossbar", "pbrick-u1-crossbar",
             "pbrick-u2-crossbar"])
        # One cable record per link, two per downstream chip, each on
        # distinct ports at both ends (108-0240-002 Figure 2-24).
        links = {}
        for cable in model["xio_cables"]:
            if not cable["id"].startswith("pbrick-"):
                continue
            ends = [components[end] for end in cable["ends"]]
            links[cable["id"]] = tuple(
                (end["parent"].removeprefix("pbrick-").removesuffix(
                    "-crossbar"), end["connector"]["port"]) for end in ends)
        self.assertEqual(links, {
            "pbrick-u0-d-to-u2-d": (("u0", 0xd), ("u2", 0xd)),
            "pbrick-u0-c-to-u2-8": (("u0", 0xc), ("u2", 0x8)),
            "pbrick-u0-9-to-u1-8": (("u0", 0x9), ("u1", 0x8)),
            "pbrick-u0-8-to-u1-d": (("u0", 0x8), ("u1", 0xd)),
        })
        self.assertEqual(components["pbrick-port-a"]["connector"]["port"],
                         0xa)
        self.assertEqual(components["pbrick-port-b"]["parent"],
                         "pbrick-u0-crossbar")

    def test_pbrick_layer_puts_two_slots_on_each_chips_buses(self):
        model = self.resolve(self.ORIGIN3200_PBRICK)
        buses = {item["id"]: item["master"] for item in model["buses"]
                 if item["id"].startswith("pbrick-pci-")}
        self.assertEqual(buses, {
            "pbrick-pci-1": "pbrick-u1", "pbrick-pci-2": "pbrick-u1",
            "pbrick-pci-3": "pbrick-u0", "pbrick-pci-4": "pbrick-u0",
            "pbrick-pci-5": "pbrick-u2", "pbrick-pci-6": "pbrick-u2",
        })
        slots = sorted((item["bus"], item["component"])
                       for item in model["connections"]
                       if item["bus"] in buses)
        self.assertEqual(slots, [
            (f"pbrick-pci-{bus}", f"pbrick-bus-{bus}-slot-{slot}")
            for bus in range(1, 7) for slot in (1, 2)])

    def test_origin3200_pbrick_hangs_the_pbrick_off_the_second_cbrick(self):
        model = self.resolve(self.ORIGIN3200_PBRICK)
        attachments = {(item["component"], item["bus"], item["role"])
                       for item in model["connections"]}
        self.assertIn(("pbrick-u0-crossbar", "c1-xio-ii", "bridge-upstream"),
                      attachments)
        self.assertIn(("c1-to-pbrick", ["c1-xio-ii-connector",
                                        "pbrick-port-a"]),
                      [(item["id"], item["ends"])
                       for item in model["xio_cables"]])
        from sgi_model.model_compile import CompileError, compile_model
        # The composition keeps the product as a reference: the emulator
        # builds no P-brick or X-brick.
        with self.assertRaisesRegex(CompileError, "model_role 'reference'"):
            compile_model(model)

    def test_origin3200_xbrick_never_compiles(self):
        from sgi_model.model_compile import CompileError, compile_model
        # The composition keeps the product as a reference: the emulator
        # builds no P-brick or X-brick.
        with self.assertRaisesRegex(CompileError, "model_role 'reference'"):
            compile_model(self.resolve(self.ORIGIN3200_XBRICK))


class Origin3900Tests(unittest.TestCase):
    """The Cx-brick layer and the Origin 3900 reference composition."""

    ORIGIN3900 = INPUTS / "origin3900.yaml"
    NODES = range(4)

    def resolve(self):
        return _resolver.load_system_model(self.ORIGIN3900)

    def components(self, model):
        return {item["id"]: item for item in model["components"]}

    def attachments(self, model):
        return {(item["component"], item["bus"], item["role"])
                for item in model["connections"]}

    def test_cxbrick_holds_four_bedrock_nodes_in_one_module(self):
        model = self.resolve()
        components = self.components(model)
        self.assertEqual(
            sorted(cid for cid, item in components.items()
                   if item["kind"] == "node-asic"),
            [f"cx0-node{n}-bedrock" for n in self.NODES])
        # One module, the brick: the four node boards are boards in it,
        # not modules of their own.
        self.assertEqual(
            sorted(cid for cid, item in components.items()
                   if item["kind"] == "module"),
            ["cx0-chassis", "ibrick-brick"])
        for n in self.NODES:
            self.assertEqual(components[f"cx0-node{n}-board"]["parent"],
                             "cx0-chassis")
            self.assertEqual(
                sorted(cid for cid, item in components.items()
                       if item["kind"] == "cpu"
                       and item["parent"] == f"cx0-node{n}-bedrock"),
                [f"cx0-node{n}-cpu{slot}" for slot in range(4)])
        # The L1's type character for a Cx-brick is unknown.
        self.assertNotIn("brick_type", components["cx0-chassis"])

    def test_cxbrick_router_has_four_external_numalink_ports(self):
        model = self.resolve()
        components = self.components(model)
        router = components["cx0-router"]
        self.assertEqual(router["router_asic"]["port_count"], 8)
        self.assertEqual(
            [components[f"cx0-numalink-{port}"]["name"]
             for port in ("a", "h", "g", "f")],
            ["NUMAlink 1 Port A", "NUMAlink 8 Port H",
             "NUMAlink 7 Port G", "NUMAlink 6 Port F"])
        buses = {item["id"]: item for item in model["buses"]}
        for port in "afgh":
            bus = buses[f"cx0-numalink-{port}-link"]
            self.assertEqual((bus["kind"], bus["master"]),
                             ("fabric", "cx0-router"))
            self.assertIn((f"cx0-numalink-{port}", bus["id"], "endpoint"),
                          self.attachments(model))

    def test_each_node_reaches_the_router_on_an_unnumbered_link(self):
        model = self.resolve()
        buses = {item["id"]: item for item in model["buses"]}
        for n in self.NODES:
            bus = buses[f"cx0-node{n}-router-link"]
            self.assertEqual((bus["kind"], bus["master"], bus["link"]),
                             ("fabric", "cx0-router", {"kind": "midplane"}))
            ends = [item for item in model["connections"]
                    if item["bus"] == bus["id"]]
            self.assertEqual(
                sorted((item["component"], item["role"]) for item in ends),
                [("cx0-node%d-bedrock" % n, "endpoint"),
                 ("cx0-router", "master")])
            # Which router link a node board uses is unknown, so no end
            # names a router port.
            self.assertTrue(all("fabric_port" not in item for item in ends))

    def test_xio_connector_n_belongs_to_node_board_n(self):
        model = self.resolve()
        components = self.components(model)
        buses = {item["id"]: item for item in model["buses"]}
        for n in self.NODES:
            connector = components[f"cx0-xio-{n}-connector"]
            self.assertEqual(connector["name"], f"XIO {n} connector")
            self.assertEqual(connector["connector"]["interface"],
                             "Crosstown2")
            bus = buses[f"cx0-node{n}-xio-ii"]
            self.assertEqual((bus["kind"], bus["master"]),
                             ("xio", f"cx0-node{n}-bedrock"))
            self.assertIn((connector["id"], bus["id"], "endpoint"),
                          self.attachments(model))

    def test_origin3900_cables_the_ibrick_to_node_0(self):
        model = self.resolve()
        self.assertEqual(
            [(item["id"], item["ends"]) for item in model["xio_cables"]],
            [("cx0-to-ibrick", ["cx0-xio-0-connector", "ibrick-port-a"])])
        self.assertIn(("ibrick-xbridge-crossbar", "cx0-node0-xio-ii",
                       "bridge-upstream"), self.attachments(model))

    def resolve_with(self, change):
        composition = yaml.safe_load(self.ORIGIN3900.read_text())
        change(composition)
        return _resolver.resolve_system_model(self.ORIGIN3900, composition)

    def test_origin3900_refuses_brick_sets_outside_its_configuration(self):
        def without_ibrick(composition):
            composition["module_instances"].pop()

        def with_rbrick(composition):
            composition["module_instances"].append({
                "model": "layers/modules/r-brick.yaml", "prefix": "rbrick-",
                "parent": "rack"})

        def two_cxbricks(composition):
            first = composition["module_instances"][0]
            composition["module_instances"].append(dict(first, prefix="cx1-"))

        for name, change, message in (
                ("no I-brick", without_ibrick,
                 "requires at least one i-brick among"),
                ("an R-brick", with_rbrick,
                 "allows at most 0 of r-brick, got 1"),
                ("two Cx-bricks", two_cxbricks,
                 "allows at most 1 of cx-brick, got 2"),
        ):
            with self.subTest(name=name), self.assertRaisesRegex(
                    _resolver.ResolutionError,
                    f"product 'origin3900' {message}"):
                self.resolve_with(change)

    def check_population(self, counts):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            shutil.copytree(INPUTS, root, dirs_exist_ok=True)
            product = root / "layers/products/origin3900.yaml"
            document = yaml.safe_load(product.read_text())
            document["layer"]["population"]["cpus_per_node"] = counts
            product.write_text(yaml.safe_dump(document, sort_keys=False))
            return _resolver.load_system_model(root / "origin3900.yaml")

    def test_cx_node_board_carries_zero_or_four_processors(self):
        model = self.check_population([4, 4, 4, 0])
        self.assertEqual(model["composition"]["population"]["cpus_per_node"],
                         [4, 4, 4, 0])
        for counts, node, count in (([4, 2, 4, 4], 1, 2),
                                    ([4, 4, 4, 3], 3, 3)):
            with self.subTest(counts=counts), self.assertRaisesRegex(
                    _resolver.ResolutionError,
                    f"node {node} populates {count} CPUs, but product "
                    "'origin3900' fits 0 or 4 per node"):
                self.check_population(counts)
        with self.assertRaisesRegex(
                _resolver.ResolutionError,
                "product 'origin3900' population has no node with "
                "processors"):
            self.check_population([0, 0, 0, 0])

    def test_origin3900_still_never_compiles(self):
        from sgi_model.model_compile import CompileError, compile_model
        with self.assertRaisesRegex(CompileError, "model_role 'reference'"):
            compile_model(self.resolve())



if __name__ == "__main__":
    unittest.main()
