# SPDX-License-Identifier: BSD-3-Clause

#!/usr/bin/env python3
"""Contract tests for system-model schema and topology validation."""

import copy
import pathlib
import sys
import unittest

import yaml

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

ROOT = pathlib.Path(__file__).resolve().parents[2]
INPUTS = ROOT / "hw/mips/sgi/models"
FIXTURES = pathlib.Path(__file__).with_name("system-model-fixtures")
sys.path.insert(0, str(ROOT / "scripts"))


def load_checker():
    from sgi_model import model_composition, model_validation
    from sgi_model.system_model import _validate_resolved_system_model
    return model_validation, _validate_resolved_system_model, model_composition


class CheckSystemModelTests(unittest.TestCase):
    def test_hardware_address_spaces_reject_emulator_metadata_and_duplicate_ids(self):
        source = self.checker_module.load_system_model(INPUTS / "origin200.yaml")
        model = copy.deepcopy(source)
        model["address_spaces"][0]["emulation"] = {"carrier_base": "0x0"}
        self.assertDocumentFails(model, "schema:")
        model = copy.deepcopy(source)
        model["address_spaces"].append(copy.deepcopy(model["address_spaces"][0]))
        self.assertDocumentFails(model, "duplicate address space id")

    @classmethod
    def setUpClass(cls):
        cls.checker_module, validate_resolved, cls.model_composition = load_checker()
        cls.validate_resolved = staticmethod(validate_resolved)

    def validation_problems(self, document):
        try:
            model = self.validate_resolved(copy.deepcopy(document))
        except self.checker_module.ResolutionError as exc:
            return [f"resolution: {exc}"]
        return self.checker_module.check(
            pathlib.Path("test model"), self.checker_module.schema(INPUTS), model=model
        )


    def test_expansion_main_power_schema_requires_both_fields(self):
        schema = self.checker_module.schema(INPUTS)
        item = schema["properties"]["machine"]["properties"]["emulation"][
            "properties"]["io_assemblies"]["items"]
        validator = self.checker_module.jsonschema.Draft202012Validator(
            dict(item, **{"$defs": schema["$defs"]}))
        entry = {
            "version": 1, "kind": "origin300-pci-expansion", "peer": "io-peer",
            "module": "io-module", "controller": "io-l1", "placement": 1,
            "initial_node_number": 0, "bedrock_revision": 2,
            "xbridge_revision": 2, "head_ports": [8, 9, 12, 13],
            "controller_profile": "pci-expansion", "bic_identity": 49152,
        }
        self.assertEqual(list(validator.iter_errors(entry)), [])
        self.assertTrue(list(validator.iter_errors(dict(
            entry, power_command_issuer="compute-l1"))))
        self.assertEqual(list(validator.iter_errors(dict(
            entry, main_power="functional-experiment", initial_main_power=True,
            power_command_issuer="compute-l1"))), [])
        for initial in (True, False):
            self.assertEqual(list(validator.iter_errors(dict(
                entry, main_power="functional-experiment",
                initial_main_power=initial))), [])
        for fields in (
            {"main_power": "functional-experiment"},
            {"initial_main_power": True},
            {"main_power": "physical", "initial_main_power": True},
            {"main_power": "functional-experiment", "initial_main_power": 1},
        ):
            self.assertTrue(list(validator.iter_errors(dict(entry, **fields))))

    def test_initial_module_number_schema(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin2000-rack.yaml")
        entry = {"component": "compute-module", "number": 73,
                 "reason": "Selected test installation."}
        model["machine"]["emulation"]["initial_module_numbers"] = [entry]
        validator = self.checker_module.jsonschema.Draft202012Validator(
            self.checker_module.schema(INPUTS)
        )
        self.assertEqual(list(validator.iter_errors(model)), [])
        for value in (True, "1", 1.5, 0, 256):
            with self.subTest(value=value):
                changed = copy.deepcopy(model)
                changed["machine"]["emulation"]["initial_module_numbers"][0][
                    "number"
                ] = value
                self.assertTrue(list(validator.iter_errors(changed)))

    def test_hub_ni_revision_override_schema(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin2000-rack.yaml")
        validator = self.checker_module.jsonschema.Draft202012Validator(
            self.checker_module.schema(INPUTS)
        )
        self.assertEqual(list(validator.iter_errors(model)), [])
        for value in (True, "5", 1.5, -1, 16):
            with self.subTest(value=value):
                changed = copy.deepcopy(model)
                changed["machine"]["emulation"]["hub_ni_revision_overrides"][0][
                    "ni_revision"
                ] = value
                self.assertTrue(list(validator.iter_errors(changed)))

    def _hub_ni_model(self, overrides):
        return {
            "machine": {"emulation": {"hub_ni_revision_overrides": overrides}},
            "components": [
                {"id": "hub0", "hardware_kind": "hub"},
                {"id": "cpu0", "hardware_kind": "cpu"},
            ],
        }

    def test_validate_hub_ni_revision_overrides_rejects_unknown_component(self):
        model = self._hub_ni_model([
            {"component": "hub-missing", "ni_revision": 1, "reason": "Field report."}
        ])
        with self.assertRaisesRegex(
            self.checker_module.ResolutionError,
            "hub NI revision override names unknown component 'hub-missing'",
        ):
            self.model_composition.validate_hub_ni_revision_overrides(model)

    def test_validate_hub_ni_revision_overrides_rejects_non_hub_component(self):
        model = self._hub_ni_model([
            {"component": "cpu0", "ni_revision": 1, "reason": "Field report."}
        ])
        with self.assertRaisesRegex(
            self.checker_module.ResolutionError,
            "component 'cpu0' is not a Hub",
        ):
            self.model_composition.validate_hub_ni_revision_overrides(model)

    def test_validate_hub_ni_revision_overrides_rejects_duplicate_component(self):
        model = self._hub_ni_model([
            {"component": "hub0", "ni_revision": 1, "reason": "Field report."},
            {"component": "hub0", "ni_revision": 2, "reason": "Second field report."},
        ])
        with self.assertRaisesRegex(
            self.checker_module.ResolutionError,
            "hub NI revision override has repeated component 'hub0'",
        ):
            self.model_composition.validate_hub_ni_revision_overrides(model)

    def test_validate_hub_ni_revision_overrides_rejects_missing_or_empty_reason(self):
        base = {"component": "hub0", "ni_revision": 1}
        for label, entry in (
            ("missing", dict(base)),
            ("empty", dict(base, reason="")),
            ("blank", dict(base, reason="   ")),
        ):
            with self.subTest(label=label):
                model = self._hub_ni_model([entry])
                with self.assertRaisesRegex(
                    self.checker_module.ResolutionError,
                    "invalid hub NI revision override entry",
                ):
                    self.model_composition.validate_hub_ni_revision_overrides(model)

    def test_board_identity_override_schema(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin2000-rack.yaml")
        model["machine"]["emulation"]["board_identity_overrides"] = [
            {"component": "baseio", "part": "030-0734-002",
             "reason": "Schema fixture."}]
        validator = self.checker_module.jsonschema.Draft202012Validator(
            self.checker_module.schema(INPUTS)
        )
        self.assertEqual(list(validator.iter_errors(model)), [])
        for value in (True, 1, ""):
            with self.subTest(value=value):
                changed = copy.deepcopy(model)
                changed["machine"]["emulation"]["board_identity_overrides"][0][
                    "part"
                ] = value
                self.assertTrue(list(validator.iter_errors(changed)))

    def _board_identity_model(self, overrides):
        return {
            "machine": {"emulation": {"board_identity_overrides": overrides}},
            "components": [
                {"id": "board0", "kind": "board"},
                {"id": "cpu0", "kind": "cpu"},
            ],
        }

    def test_validate_board_identity_overrides_rejects_unknown_component(self):
        model = self._board_identity_model([
            {"component": "board-missing", "part": "030-0734-002", "reason": "Field report."}
        ])
        with self.assertRaisesRegex(
            self.checker_module.ResolutionError,
            "board identity override names unknown component 'board-missing'",
        ):
            self.model_composition.validate_board_identity_overrides(model)

    def test_validate_board_identity_overrides_rejects_non_board_component(self):
        model = self._board_identity_model([
            {"component": "cpu0", "part": "030-0734-002", "reason": "Field report."}
        ])
        with self.assertRaisesRegex(
            self.checker_module.ResolutionError,
            "component 'cpu0' is not a board",
        ):
            self.model_composition.validate_board_identity_overrides(model)

    def test_validate_board_identity_overrides_rejects_duplicate_component(self):
        model = self._board_identity_model([
            {"component": "board0", "part": "030-0734-002", "reason": "Field report."},
            {"component": "board0", "part": "030-1124-002", "reason": "Second field report."},
        ])
        with self.assertRaisesRegex(
            self.checker_module.ResolutionError,
            "board identity override has repeated component 'board0'",
        ):
            self.model_composition.validate_board_identity_overrides(model)

    def test_validate_board_identity_overrides_rejects_missing_or_empty_reason(self):
        base = {"component": "board0", "part": "030-0734-002"}
        for label, entry in (
            ("missing", dict(base)),
            ("empty", dict(base, reason="")),
            ("blank", dict(base, reason="   ")),
        ):
            with self.subTest(label=label):
                model = self._board_identity_model([entry])
                with self.assertRaisesRegex(
                    self.checker_module.ResolutionError,
                    "invalid board identity override entry",
                ):
                    self.model_composition.validate_board_identity_overrides(model)

    def test_validate_board_identity_overrides_rejects_missing_or_empty_part(self):
        base = {"component": "board0", "reason": "Field report."}
        for label, entry in (
            ("missing", dict(base)),
            ("empty", dict(base, part="")),
            ("blank", dict(base, part="   ")),
        ):
            with self.subTest(label=label):
                model = self._board_identity_model([entry])
                with self.assertRaisesRegex(
                    self.checker_module.ResolutionError,
                    "invalid board identity override entry",
                ):
                    self.model_composition.validate_board_identity_overrides(model)
    def _slot_model(self, slot):
        return {"components": [
            {"id": "module0", "kind": "module"},
            {"id": "board0", "kind": "board", "parent": "module0",
             "physical_slot": slot},
        ]}

    def test_validate_physical_slots_accepts_hub_slot_bits_on_a_node_slot(self):
        self.model_composition.validate_physical_slots(
            self._slot_model({"kind": "node", "number": 1, "hub_slot_bits": 3}))

    def test_validate_physical_slots_rejects_hub_slot_bits_off_a_node_slot(self):
        for label, slot in (
            ("io slot", {"kind": "io", "number": 1, "hub_slot_bits": 3}),
            ("too wide", {"kind": "node", "number": 1, "hub_slot_bits": 8}),
        ):
            with self.subTest(label=label):
                with self.assertRaisesRegex(
                    self.checker_module.ResolutionError,
                    "component 'board0' names Hub slot bits outside a node "
                    "slot's 0 to 7",
                ):
                    self.model_composition.validate_physical_slots(
                        self._slot_model(slot))

    def _widget_identity_model(self, identities):
        # Minimal routed-fabric model (one router-asic is enough for
        # `_fabric_topology` to read "router") carrying one bridge-less XIO
        # widget board (XG-style, `hardware_kind: "xg"`), the same
        # identity-bearing component `model_compile._XIO_WIDGET_KINDS`
        # names for the compiler.
        return {
            "components": [
                {"id": "router0", "kind": "router-asic"},
                {"id": "widget0", "kind": "io-controller", "hardware_kind": "xg"},
            ],
            "machine": {"emulation": {"synthetic_identities": identities}},
        }

    def test_validate_synthetic_identities_accepts_a_widget_with_identity(self):
        model = self._widget_identity_model([
            {"component": "router0", "serial": "0x1", "reason": "Fixture router."},
            {"component": "widget0", "serial": "0x2", "reason": "Fixture widget."},
        ])
        self.model_composition.validate_synthetic_identities(model)

    def test_validate_synthetic_identities_rejects_a_widget_missing_identity(self):
        model = self._widget_identity_model([
            {"component": "router0", "serial": "0x1", "reason": "Fixture router."},
        ])
        with self.assertRaisesRegex(
            self.checker_module.ResolutionError,
            "missing synthetic identity for 'widget0'",
        ):
            self.model_composition.validate_synthetic_identities(model)

    def test_module_identity_schema_matches_compiler_value_limits(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin2000-rack.yaml"
        )
        validator = self.checker_module.jsonschema.Draft202012Validator(
            self.checker_module.schema(INPUTS)
        )

        accepted = copy.deepcopy(model)
        accepted["machine"]["emulation"]["module_semantic_serials"][0][
            "serial"
        ] = "OK=SERIAL1"
        accepted["machine"]["emulation"]["module_responder_identities"][0][
            "serial"
        ] = "0xffff_ffff_ffff"
        self.assertEqual(list(validator.iter_errors(accepted)), [])

        for serial in ("BAD:SERIAL", "BAD;SERIAL"):
            with self.subTest(serial=serial):
                changed = copy.deepcopy(model)
                changed["machine"]["emulation"]["module_semantic_serials"][0][
                    "serial"
                ] = serial
                self.assertTrue(list(validator.iter_errors(changed)))

        changed = copy.deepcopy(model)
        changed["machine"]["emulation"]["module_responder_identities"][0][
            "serial"
        ] = "0x1_0000_0000_0000"
        self.assertTrue(list(validator.iter_errors(changed)))

    def test_origin300_ioc3_mii_phy_identity(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin300.yaml"
        )
        validator = self.checker_module.jsonschema.Draft202012Validator(
            self.checker_module.schema(INPUTS)
        )
        self.assertEqual(list(validator.iter_errors(model)), [])

        for phy in (
            {"address": 0, "part": "dp83843"},
            {"address": 1, "part": "dp83840"},
            {"address": 1},
            {"part": "dp83843"},
        ):
            with self.subTest(phy=phy):
                changed = copy.deepcopy(model)
                next(item for item in changed["components"] if item["id"] == "ioc3")[
                    "identity"
                ]["mii_phy"] = phy
                self.assertTrue(list(validator.iter_errors(changed)))

    def test_r10000_reset_mode_schema_accepts_fields_and_rejects_reserved_values(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin2000-rack.yaml"
        )
        self.assertDocumentPasses(model)

        mode = model["machine"]["firmware_config"]["processor_reset_mode"]
        for field, value in (
            ("processor_device_number", 4),
            ("secondary_cache_clock_divisor", 3.5),
            ("secondary_cache_clock_tap", 6),
        ):
            with self.subTest(field=field):
                changed = copy.deepcopy(model)
                changed["machine"]["firmware_config"]["processor_reset_mode"][field] = value
                self.assertDocumentFails(changed, "schema:")

    def test_shared_i2c_master_list_passes_schema_and_references(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        bus = next(item for item in model["buses"] if item["id"] == "i2c")
        del bus["master"]
        bus["masters"] = ["pcf8584", "sgi-elsc"]
        for connection in model["connections"]:
            if connection["bus"] == "i2c" and connection["component"] == "sgi-elsc":
                connection["role"] = "master"
        self.assertDocumentPasses(model)

    def assertDocumentFails(self, document, message):
        self.assertIn(message, "; ".join(self.validation_problems(document)))

    def assertDocumentPasses(self, document):
        self.assertEqual(self.validation_problems(document), [])

    def fixture(self, name):
        return yaml.safe_load((FIXTURES / name).read_text())

    def unknown_memory_reference(self):
        model = self.fixture("population-orientation.yaml")
        population = model["composition"]["population"]
        del population["memory_mib_per_node"]
        population["fitted_memory"] = {
            "status": "unknown",
            "reason": "The source proves slot capacity but not a fitted memory configuration.",
            "provenance": "vendor-document",
            "sources": ["Origin 2000 service manual"],
        }
        limits = model["composition"]["limits"]
        limits["dimms_per_node"] = 16
        limits["provenance"] = "vendor-document"
        limits["sources"] = ["Origin 2000 service manual"]
        return model

    def assertSchemaPasses(self, name):
        document = self.fixture(name)
        errors = list(self.checker_module.jsonschema.Draft202012Validator(
            self.checker_module.schema(INPUTS)
        ).iter_errors(document))
        self.assertEqual(errors, [])

    def assertSchemaFails(self, name):
        document = self.fixture(name)
        errors = list(self.checker_module.jsonschema.Draft202012Validator(
            self.checker_module.schema(INPUTS)
        ).iter_errors(document))
        self.assertTrue(errors)

    def assertCheckFails(self, name, message):
        self.assertIn(message, "; ".join(
            self.validation_problems(self.fixture(name))
        ))

    def test_emulation_target_requires_emulation_mapping(self):
        self.assertSchemaFails("target-without-emulation.yaml")

    def test_component_identity_rejects_generic_revision(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        cpu = next(component for component in model["components"]
                   if component["id"] == "cpu0")
        cpu["identity"]["revision"] = 2
        self.assertDocumentFails(model, "schema:")

    def test_component_hardware_kind_is_an_identifier(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml"
        )
        cpu = next(component for component in model["components"]
                   if component["id"] == "cpu0")
        cpu["hardware_kind"] = "r10000"
        self.assertDocumentPasses(model)

        cpu["hardware_kind"] = "R10000"
        self.assertDocumentFails(model, "schema:")

    def test_product_cannot_duplicate_assembly_memory_bank_rules(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml"
        )
        model["machine"]["memory_banks"] = {
            "physical_banks": 4,
            "supported_bank_sizes_mib": [64, 128, 256, 512],
        }

        self.assertDocumentFails(model, "schema: machine")

    def test_machine_rejects_repeated_node_count(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml"
        )
        model["machine"]["nodes"] = model["composition"]["population"]["nodes"]

        self.assertDocumentFails(model, "schema: machine")

    def test_emulation_target_requires_emulation_profile(self):
        model = self.fixture("direct-xio.yaml")
        model["machine"]["emulation"].pop("profile", None)

        self.assertDocumentFails(
            model, "emulation target requires a profile identifier"
        )

    def test_schema_requires_emulation_profile(self):
        model = self.fixture("direct-xio.yaml")
        model["machine"]["emulation"].pop("profile", None)
        schema = yaml.safe_load(
            (INPUTS / "system-model.schema.json").read_text()
        )

        errors = list(
            self.checker_module.jsonschema.Draft202012Validator(schema).iter_errors(model)
        )

        self.assertTrue(
            any("'profile' is a required property" in error.message
                for error in errors),
            [error.message for error in errors],
        )





    def test_public_checker_does_not_require_datasheet_contract_paths(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin300.yaml"
        )
        bedrock = next(
            component for component in model["components"]
            if component["id"] == "bedrock"
        )
        bedrock["datasheet_contracts"] = [
            "docs/datasheets/not-present-in-this-checkout.md"
        ]
        self.assertDocumentPasses(model)

    def test_datasheet_contracts_keep_nonempty_unique_path_shape(self):
        model = self.checker_module.load_system_model(INPUTS / "origin200.yaml")
        hub = next(component for component in model["components"]
                   if component["id"] == "hub")
        original = hub["datasheet_contracts"]
        for paths in ([], [original[0], original[0]], ["datasheets/missing.md"]):
            with self.subTest(paths=paths):
                changed = copy.deepcopy(model)
                next(component for component in changed["components"]
                     if component["id"] == "hub")["datasheet_contracts"] = paths
                self.assertDocumentFails(changed, "schema:")

    def test_implemented_target_requires_compiler_inputs_in_schema(self):
        removals = {
            "emulator constraints": lambda model: model["machine"][
                "emulation"
            ].pop("emulator_constraints"),
            "firmware config": lambda model: model["machine"].pop(
                "firmware_config"
            ),
        }
        for label, remove in removals.items():
            with self.subTest(label=label):
                model = self.checker_module.load_system_model(
                    INPUTS / "origin200.yaml"
                )
                remove(model)
                self.assertDocumentFails(model, "schema:")

    def test_schema_accepts_module_roles(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200-dual.yaml"
        )
        for role, module in zip(
                ("MASTER", "SLAVE"),
                model["composition"]["module_instances"]):
            module["module_role"] = role

        self.assertDocumentPasses(model)

    def test_other_implemented_machine_does_not_require_origin200_topology(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml"
        )
        model["machine"]["emulation"]["machine_name"] = "other"

        self.assertDocumentPasses(model)

    def test_direct_origin200_fields_reject_invalid_values(self):
        def add_direct_fields(model):
            hub_sysad = next(
                connection for connection in model["connections"]
                if connection.get("component") == "hub"
                and connection.get("bus") == "sysad"
            )
            hub_sysad["cpu_interrupt_inputs"] = {
                "bank0": 0, "bank1": 1, "rt": 2,
            }
            if not any(bus.get("id") == "i2c" for bus in model["buses"]):
                model["buses"].append({
                    "id": "i2c",
                    "name": "I2C",
                    "kind": "i2c",
                    "master": "pcf8584",
                    "provenance": "vendor-document",
                    "sources": ["I2C test fixture"],
                })
            direct_connections = (
                {
                    "component": "pcf8584", "bus": "i2c", "role": "master",
                    "provenance": "vendor-document",
                    "sources": ["I2C test fixture"],
                },
                {
                    "component": "sgi-elsc", "bus": "i2c", "role": "endpoint",
                    "i2c_address": 0x08,
                    "provenance": "vendor-document",
                    "sources": ["I2C test fixture"],
                },
                {
                    "component": "i2c-nvram", "bus": "i2c", "role": "endpoint",
                    "i2c_address": 0x50,
                    "i2c_address_count": 8,
                    "provenance": "vendor-document",
                    "sources": ["I2C test fixture"],
                },
            )
            for connection in direct_connections:
                if not any(
                        existing.get("component") == connection["component"]
                        and existing.get("bus") == "i2c"
                        for existing in model["connections"]):
                    model["connections"].append(connection)

        def missing_i2c_endpoint_address(model):
            connection = next(
                connection for connection in model["connections"]
                if connection.get("component") == "i2c-nvram"
                and connection.get("bus") == "i2c"
            )
            connection.pop("i2c_address")

        def missing_i2c_endpoints(model):
            model["connections"] = [
                connection for connection in model["connections"]
                if not (
                    connection.get("bus") == "i2c"
                    and connection.get("role") == "endpoint"
                )
            ]

        def i2c_span_outside_seven_bit_address_space(model):
            connection = next(
                connection for connection in model["connections"]
                if connection.get("component") == "i2c-nvram"
                and connection.get("bus") == "i2c"
            )
            connection["i2c_address_count"] = 49

        def overlapping_i2c_endpoints(model):
            connection = next(
                connection for connection in model["connections"]
                if connection.get("component") == "sgi-elsc"
                and connection.get("bus") == "i2c"
            )
            connection["i2c_address"] = 0x50

        def cpu_input_above_five(model):
            connection = next(
                connection for connection in model["connections"]
                if connection.get("component") == "hub"
                and connection.get("bus") == "sysad"
            )
            connection["cpu_interrupt_inputs"]["rt"] = 6

        cases = (
            (missing_i2c_endpoints, "I2C bus 'i2c' has no endpoints"),
            (missing_i2c_endpoint_address, "I2C endpoint 'i2c-nvram' has no address"),
            (i2c_span_outside_seven_bit_address_space,
             "I2C endpoint 'i2c-nvram' span exceeds 7-bit address space"),
            (overlapping_i2c_endpoints,
             "I2C address 0x50 on bus 'i2c'"),
            (cpu_input_above_five, "schema:"),
        )
        for mutate, message in cases:
            with self.subTest(message=message):
                model = self.checker_module.load_system_model(
                    INPUTS / "origin200.yaml"
                )
                model["machine"]["emulation"]["profile"] = "schema-fixture"
                add_direct_fields(model)
                mutate(model)
                self.assertDocumentFails(model, message)

    def test_removed_origin200_fields_are_rejected(self):
        def semantic_bindings(model):
            model["machine"]["emulation"]["semantic_bindings"] = {}

        def runtime_topology(model):
            model["machine"]["emulation"]["runtime_topology"] = {
                "junk_uart": {
                    "kind": "HUB_UART",
                    "node": 0,
                    "default_enabled": False,
                },
            }

        for mutate in (semantic_bindings, runtime_topology):
            with self.subTest(mutate=mutate.__name__):
                model = self.checker_module.load_system_model(
                    INPUTS / "origin200.yaml"
                )
                model["machine"]["emulation"]["profile"] = "schema-fixture"
                mutate(model)
                self.assertDocumentFails(model, "schema:")

    def test_optional_component_outside_supported_profile_is_rejected(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml"
        )
        junk_uart = next(
            component for component in model["components"]
            if component["id"] == "junk-uart"
        )
        junk_uart["optional"]["node"] = 1

        self.assertDocumentFails(
            model,
            "optional component 'junk-uart' names node 1 outside every "
            "supported profile",
        )

    def test_optional_component_bounds_use_topology_supported_populations(self):
        # origin200-dual carries no machine.emulation.accepted_cpu_populations
        # (only origin200 ever did); the bounds check must read the one
        # remaining list, composition.topology.supported_cpu_populations,
        # for every product, not only the one that used to duplicate it.
        model = self.checker_module.load_system_model(
            INPUTS / "origin200-dual.yaml"
        )
        junk_uart = next(
            component for component in model["components"]
            if component["id"] == "node0-junk-uart"
        )
        junk_uart["optional"]["node"] = 5

        self.assertDocumentFails(
            model,
            "optional component 'node0-junk-uart' names node 5 outside every "
            "supported profile",
        )

    def test_emulator_bank_size_must_fit_a_16_bit_field(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml"
        )
        model["machine"]["emulation"]["emulator_constraints"][
            "accepted_bank_sizes_mib"
        ] = [65536]

        self.assertDocumentFails(model, "schema:")

    def test_target_component_rejects_implementation_inventory(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml"
        )
        model["components"][0]["modelled"] = "full"

        self.assertDocumentFails(model, "schema:")

    def test_target_interrupt_rejects_implementation_inventory(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml"
        )
        model["interrupts"][0]["implementation_status"] = "implemented"
        model["interrupts"][0]["implementation_sources"] = ["qtest"]

        self.assertDocumentFails(model, "schema:")



    def test_module_instance_observation_requires_its_own_basis(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200-dual.yaml"
        )
        model["composition"]["module_instances"][0].pop(
            "observed_hardware", None
        )

        self.assertDocumentFails(
            model,
            "module instance 'node0-' retains observed-hardware claims without a direct instance capture",
        )

    def test_reference_machine_omits_emulation_mapping(self):
        self.assertSchemaPasses("reference-machine.yaml")

    def test_address_architecture_facts_accept_sources(self):
        model = self.fixture("reference-machine.yaml")
        model["space_selection"].update({
            "provenance": "public-header",
            "sources": ["Public processor architecture manual"],
        })
        model["address_spaces"][0].update({
            "provenance": "public-header",
            "sources": ["Public platform address header"],
        })

        self.assertDocumentPasses(model)


    def test_reference_router_asic_owns_six_fabric_links(self):
        self.assertSchemaPasses("router-reference.yaml")

    def test_shared_fabric_bus_counts_as_one_attachment_for_each_router(self):
        model = self.fixture("router-reference.yaml")
        model["components"].extend([
            {
                "id": "router-board-two",
                "name": "Router board two",
                "kind": "board",
                "provenance": "vendor-document",
                "sources": ["Origin Router fixture source"],
            },
            {
                "id": "router-two",
                "name": "Router two",
                "kind": "router-asic",
                "parent": "router-board-two",
                "provenance": "vendor-document",
                "sources": ["Origin Router fixture source"],
                "router_asic": {"port_count": 6, "role": "router"},
            },
        ])
        for index in range(5):
            model["buses"].append({
                "id": f"router-two-fabric-{index}",
                "name": f"Router two fabric {index}",
                "kind": "fabric",
                "master": "router-two",
                "provenance": "vendor-document",
                "sources": ["Origin Router fixture source"],
            })
        model["connections"].append({
            "component": "router-two",
            "bus": "fabric-five",
            "role": "endpoint",
            "provenance": "vendor-document",
            "sources": ["Origin Router fixture source"],
        })

        self.assertDocumentPasses(model)

    def test_router_asic_allows_fewer_than_six_connected_links(self):
        model = self.fixture("router-reference.yaml")
        model["buses"][5]["kind"] = "internal"

        self.assertDocumentPasses(model)

    def test_router_asic_rejects_more_than_six_connected_links(self):
        model = self.fixture("router-reference.yaml")
        model["buses"].append({
            "id": "fabric-six",
            "name": "CrayLink 6",
            "kind": "fabric",
            "master": "router",
            "provenance": "vendor-document",
            "sources": ["Origin Router fixture source"],
        })

        self.assertDocumentFails(
            model,
            "router-asic 'router' must not own more than 6 fabric links, got 7",
        )

    def test_router_asic_requires_port_count_and_role(self):
        model = self.fixture("router-reference.yaml")
        del model["components"][1]["router_asic"]
        self.assertDocumentFails(model, "schema:")

    def test_router_asic_rejects_unknown_role(self):
        model = self.fixture("router-reference.yaml")
        model["components"][1]["router_asic"]["role"] = "hub"
        self.assertDocumentFails(model, "schema:")

    def test_router_asic_rejects_more_links_than_its_own_port_count(self):
        model = self.fixture("router-reference.yaml")
        model["components"][1]["router_asic"]["port_count"] = 4
        model["buses"][5]["kind"] = "internal"

        self.assertDocumentFails(
            model,
            "router-asic 'router' must not own more than 4 fabric links, got 5",
        )

    def test_router_asic_still_requires_valid_containment(self):
        model = self.fixture("router-reference.yaml")
        model["components"][1]["parent"] = "missing-router-board"

        self.assertDocumentFails(
            model,
            "component 'router' parent: no such component 'missing-router-board'",
        )

    def test_fabric_port_is_accepted_on_a_fabric_connection(self):
        model = self.fixture("router-reference.yaml")
        model["connections"][0]["fabric_port"] = 1
        self.assertDocumentPasses(model)

    def test_fabric_port_requires_a_fabric_bus(self):
        model = self.fixture("router-reference.yaml")
        model["buses"].append({
            "id": "sysad", "name": "SysAD", "kind": "sysad", "master": "router",
            "provenance": "vendor-document",
            "sources": ["Origin Router fixture source"],
        })
        model["connections"].append({
            "component": "router", "bus": "sysad", "role": "endpoint",
            "fabric_port": 1, "provenance": "vendor-document",
            "sources": ["Origin Router fixture source"],
        })
        self.assertDocumentFails(
            model, "fabric_port is only valid on a fabric bus")

    def test_fabric_port_must_be_unique_per_component(self):
        model = self.fixture("router-reference.yaml")
        for bus in ("fabric-zero", "fabric-one"):
            model["connections"].append({
                "component": "router", "bus": bus, "role": "master",
                "fabric_port": 6, "provenance": "vendor-document",
                "sources": ["Origin Router fixture source"],
            })
        self.assertDocumentFails(model, "already used by connection")

    def test_fabric_ports_may_differ_across_a_components_links(self):
        model = self.fixture("router-reference.yaml")
        for bus, port in (("fabric-zero", 5), ("fabric-one", 4)):
            model["connections"].append({
                "component": "router", "bus": bus, "role": "master",
                "fabric_port": port, "provenance": "vendor-document",
                "sources": ["Origin Router fixture source"],
            })
        self.assertDocumentPasses(model)

    def test_two_components_may_share_a_fabric_port_number(self):
        model = self.fixture("router-reference.yaml")
        model["connections"][0]["fabric_port"] = 6
        model["connections"][1]["fabric_port"] = 6
        self.assertDocumentPasses(model)

    def _link_bus_fixture(self):
        """router-reference with fabric-zero given a second attachment."""
        model = self.fixture("router-reference.yaml")
        model["connections"].append({
            "component": "router", "bus": "fabric-zero", "role": "master",
            "provenance": "vendor-document",
            "sources": ["Origin Router fixture source"],
        })
        return model

    def test_link_bus_accepts_two_connections(self):
        model = self._link_bus_fixture()
        model["buses"][0]["link"] = {"kind": "midplane"}
        self.assertDocumentPasses(model)

    def test_link_requires_a_fabric_bus(self):
        # The schema, not check(), rejects this: a bus with kind other than
        # "fabric" cannot carry a link at all.
        for kind in ("pci", "sysad"):
            with self.subTest(kind=kind):
                model = self._link_bus_fixture()
                model["buses"][0]["kind"] = kind
                model["buses"][0]["link"] = {"kind": "midplane"}
                self.assertDocumentFails(
                    model, "schema: buses/0/kind: 'fabric' was expected")

    def test_link_bus_requires_two_connections(self):
        model = self.fixture("router-reference.yaml")
        model["buses"][0]["link"] = {"kind": "midplane"}
        self.assertDocumentFails(
            model, "link bus 'fabric-zero' joins exactly two ports, got 1")

    def test_link_bus_rejects_three_connections(self):
        model = self._link_bus_fixture()
        model["connections"].append({
            "component": "router", "bus": "fabric-zero", "role": "master",
            "provenance": "vendor-document",
            "sources": ["Origin Router fixture source"],
        })
        model["buses"][0]["link"] = {"kind": "midplane"}
        self.assertDocumentFails(
            model, "link bus 'fabric-zero' joins exactly two ports, got 3")

    def test_cable_link_carries_part_length_and_colour(self):
        model = self._link_bus_fixture()
        model["buses"][0]["link"] = {
            "kind": "cable", "part": "018-0564-001", "length_in": 58,
            "color": "red",
        }
        self.assertDocumentPasses(model)

    def test_cable_fields_require_a_cable_kind(self):
        model = self._link_bus_fixture()
        model["buses"][0]["link"] = {"kind": "midplane", "part": "018-0564-001"}
        self.assertDocumentFails(
            model, "kind 'midplane' cannot carry cable field 'part'")

    def test_unknown_link_kind_is_rejected(self):
        model = self._link_bus_fixture()
        model["buses"][0]["link"] = {"kind": "wormhole"}
        self.assertDocumentFails(model, "schema:")

    def test_empty_connector_is_a_physical_endpoint(self):
        self.assertSchemaPasses("reference-machine.yaml")

    def test_connector_kind_requires_endpoint_metadata(self):
        model = self.fixture("reference-machine.yaml")
        del model["components"][2]["connector"]

        self.assertDocumentFails(model, "connector")

    def test_non_connector_rejects_endpoint_metadata(self):
        model = self.fixture("reference-machine.yaml")
        model["components"][1]["connector"] = {
            "interface": "XIO",
            "occupancy": "empty",
        }

        self.assertDocumentFails(model, "connector")

    def test_connector_must_attach_with_endpoint_role(self):
        model = self.fixture("reference-machine.yaml")
        model["connections"][1]["role"] = "master"

        self.assertDocumentFails(model, "connector 'xio-slot-11' must use endpoint role")

    def test_connector_must_have_a_bus_attachment(self):
        model = self.fixture("reference-machine.yaml")
        del model["connections"][1]

        self.assertDocumentFails(model, "connector 'xio-slot-11' has no bus attachment")

    def test_unknown_cpu_orientation_is_not_an_ordered_node_population(self):
        self.assertSchemaPasses("population-orientation.yaml")

    def test_checked_model_requires_strict_evidence_policy(self):
        model = self.fixture("population-orientation.yaml")
        del model["evidence_policy"]
        self.assertDocumentFails(model, "evidence_policy")

    def test_checked_model_rejects_retired_provenance_tags(self):
        for provenance in ("survey", "observed", "guest-confirmed"):
            with self.subTest(provenance=provenance):
                model = self.fixture("reference-machine.yaml")
                model["components"][0]["provenance"] = provenance
                self.assertDocumentFails(model, "provenance")

    def test_strict_composed_population_requires_supported_set(self):
        self.assertCheckFails(
            "strict-composed-no-supported-population.yaml",
            "supported_cpu_populations is required",
        )

    def test_reference_population_uses_product_and_assembly_limits(self):
        model = self.fixture("strict-composed-no-supported-population.yaml")
        model["machine"]["model_role"] = "reference"
        del model["machine"]["emulation"]

        self.assertDocumentPasses(model)

    def test_declared_supported_set_checks_selected_population(self):
        model = self.fixture("strict-composed-no-supported-population.yaml")
        model["composition"]["topology"]["supported_cpu_populations"] = [[2]]

        self.assertDocumentFails(model, "selected CPU population [1] is not supported")

    def test_supported_population_total_matches_node_counts(self):
        model = self.fixture("population-orientation.yaml")
        model["composition"]["topology"]["supported_cpu_populations"][0][
            "total_cpus"
        ] = 2

        self.assertDocumentFails(model, "total_cpus 2 does not match node_counts total 3")

    def test_supported_population_node_count_matches_counts(self):
        model = self.fixture("population-orientation.yaml")
        model["composition"]["topology"]["supported_cpu_populations"][0]["nodes"] = 3

        self.assertDocumentFails(model, "nodes 3 does not match 2 node_counts entries")

    def test_supported_population_respects_assembly_capacity(self):
        model = self.fixture("population-orientation.yaml")
        population = model["composition"]["topology"]["supported_cpu_populations"][0]
        population["total_cpus"] = 4
        population["node_counts"] = [1, 3]

        self.assertDocumentFails(model, "node count 3 exceeds the 2-CPU assembly capacity")

    def test_selected_population_requires_a_supported_population_contract(self):
        model = self.fixture("population-orientation.yaml")
        del model["composition"]["topology"]["supported_cpu_populations"]
        model["machine"]["model_role"] = "emulation-target"

        self.assertDocumentFails(model, "supported_cpu_populations is required")

    def test_supported_population_respects_minimum_nodes(self):
        model = self.fixture("population-orientation.yaml")
        topology = model["composition"]["topology"]
        topology["min_nodes"] = 2
        topology["supported_cpu_populations"].append([1])

        self.assertDocumentFails(model, "uses 1 nodes, but the topology requires at least 2")

    def test_supported_population_respects_maximum_nodes(self):
        model = self.fixture("population-orientation.yaml")
        model["composition"]["topology"]["supported_cpu_populations"].append([1, 1, 1])

        self.assertDocumentFails(model, "uses 3 nodes, but the topology permits at most 2")

    def test_selected_population_must_be_supported_by_hardware(self):
        model = self.fixture("direct-xio.yaml")
        population = model["composition"]["population"]
        population["nodes"] = 2
        population["cpus_per_node"] = [1, 1]
        population["memory_mib_per_node"] = [64, 64]

        self.assertDocumentFails(model, "selected CPU population [1, 1] is not supported")

    def test_strict_reference_accepts_unknown_fitted_memory(self):
        self.assertDocumentPasses(self.unknown_memory_reference())

    def test_root_population_cannot_duplicate_composed_population(self):
        model = self.unknown_memory_reference()
        model["population"] = dict(model["composition"]["population"])

        self.assertDocumentFails(
            model,
            "model cannot contain both root population and composition",
        )

    def test_zero_is_not_an_unknown_memory_sentinel(self):
        model = self.fixture("population-orientation.yaml")
        model["composition"]["population"]["memory_mib_per_node"] = [0, 0]

        self.assertDocumentFails(model, "memory_mib_per_node must contain positive MiB values")

    def test_target_memory_population_matches_selected_nodes(self):
        model = self.fixture("direct-xio.yaml")
        model["composition"]["population"]["memory_mib_per_node"] = []

        self.assertDocumentFails(model, "memory_mib_per_node has 0 entries for 1 nodes")

    def test_target_cannot_use_unknown_fitted_memory(self):
        model = self.fixture("direct-xio.yaml")
        population = model["composition"]["population"]
        del population["memory_mib_per_node"]
        population["fitted_memory"] = {
            "status": "unknown",
            "reason": "No target memory was selected.",
            "provenance": "derived",
            "sources": ["Task 5 target memory fixture"],
        }

        self.assertDocumentFails(model, "emulation target requires a concrete fitted memory population")

    def test_positive_selected_memory_remains_valid(self):
        self.assertDocumentPasses(self.fixture("population-orientation.yaml"))


    def test_malformed_unknown_memory_state_is_rejected(self):
        model = self.unknown_memory_reference()
        model["composition"]["population"]["fitted_memory"]["status"] = "maybe"

        self.assertDocumentFails(model, "status")

    def test_unknown_memory_state_requires_evidence(self):
        model = self.unknown_memory_reference()
        del model["composition"]["population"]["fitted_memory"]["sources"]

        self.assertDocumentFails(model, "strict evidence policy requires sources for fitted memory")

    def test_strict_reference_requires_a_memory_selection_state(self):
        model = self.fixture("population-orientation.yaml")
        del model["composition"]["population"]["memory_mib_per_node"]

        self.assertDocumentFails(model, "strict model requires concrete or unknown fitted memory")

    def test_strict_capacity_limits_require_evidence(self):
        model = self.unknown_memory_reference()
        del model["composition"]["limits"]["sources"]

        self.assertDocumentFails(model, "strict evidence policy requires sources for capacity limits")

    def test_topology_requires_evidence(self):
        model = self.fixture("population-orientation.yaml")
        del model["composition"]["topology"]["sources"]

        self.assertDocumentFails(model, "sources")

    def test_selected_population_requires_evidence(self):
        model = self.fixture("population-orientation.yaml")
        del model["composition"]["population"]["sources"]

        self.assertDocumentFails(model, "sources")

    def test_bus_requires_evidence(self):
        model = self.fixture("reference-machine.yaml")
        del model["buses"][0]["sources"]

        self.assertDocumentFails(model, "sources")

    def test_connection_requires_evidence(self):
        model = self.fixture("reference-machine.yaml")
        del model["connections"][0]["sources"]

        self.assertDocumentFails(model, "sources")

    def test_strict_evidence_rejects_unsourced_bus_gap(self):
        model = self.fixture("reference-machine.yaml")
        bus = model["buses"][0]
        bus["provenance"] = "unsourced"
        del bus["sources"]
        bus["evidence_gap"] = "The physical bus source has not been reviewed."

        self.assertDocumentFails(model, "strict evidence policy rejects unsourced bus 'xio'")

    def test_strict_evidence_rejects_unsourced_topology_gap(self):
        model = self.fixture("population-orientation.yaml")
        topology = model["composition"]["topology"]
        topology["provenance"] = "unsourced"
        del topology["sources"]
        topology["evidence_gap"] = "The node topology is unresolved."

        self.assertDocumentFails(model, "strict evidence policy rejects unsourced topology")

    def test_strict_evidence_rejects_unsourced_population_gap(self):
        model = self.fixture("population-orientation.yaml")
        population = model["composition"]["population"]
        population["provenance"] = "unsourced"
        del population["sources"]
        population["evidence_gap"] = "The selected population is unresolved."

        self.assertDocumentFails(model, "strict evidence policy rejects unsourced population")

    def test_connector_selection_rejects_invalid_provenance(self):
        for provenance, message in (
                ("unsourced", "strict evidence policy rejects unsourced connector population"),
                ("invented-tag", "is not one of")):
            with self.subTest(provenance=provenance):
                model = self.checker_module.load_system_model(
                    INPUTS / "origin2000-rack.yaml")
                model["composition"]["connector_population"][0]["provenance"] = provenance
                self.assertDocumentFails(model, message)

    def test_strict_evidence_rejects_unsourced_connection_port(self):
        model = self.fixture("reference-machine.yaml")
        connection = model["connections"][1]
        connection["xio"]["physical_port"] = 12
        connection["provenance"] = "unsourced"
        del connection["sources"]
        connection["evidence_gap"] = "No source has established this endpoint."

        self.assertDocumentFails(model, "strict evidence policy rejects unsourced connection 1")

    def test_strict_evidence_rejects_unsourced_interrupt_gap(self):
        model = self.fixture("direct-xio.yaml")
        interrupt = model["interrupts"][0]
        interrupt["provenance"] = "unsourced"
        del interrupt["sources"]
        interrupt["evidence_gap"] = "The interrupt route is unresolved."

        self.assertDocumentFails(model, "strict evidence policy rejects unsourced interrupt 'scsi0'")

    def test_strict_emulation_target_firmware_config_requires_sources(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml"
        )
        model["machine"]["firmware_config"].pop("sources", None)

        self.assertDocumentFails(model, "schema:")

    def test_strict_evidence_rejects_unsourced_firmware_payload(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml"
        )
        model["machine"]["emulation"]["firmware_payload"] = {
            "argument": "-bios",
            "path": "firmware/test/prom.payload.bin",
            "size": "0x20",
            "sha256": "1" * 64,
            "read_only": True,
            "views": [{
                "name": "reset view",
                "address": "0x1fc0_0000",
                "payload_offset": "0x0",
                "size": "0x20",
            }],
            "provenance": "unsourced",
            "sources": ["Unverified placeholder"],
            "notes": "No evidence establishes this projection.",
        }

        self.assertDocumentFails(
            model,
            "strict evidence policy rejects unsourced emulator firmware payload",
        )

    def test_interrupt_path_requires_hardware_evidence(self):
        model = self.fixture("direct-xio.yaml")
        del model["interrupts"][0]["sources"]

        self.assertDocumentFails(model, "sources")

    def test_firmware_window_must_not_overlap_supported_memory(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml"
        )
        flash = next(component for component in model["components"]
                     if component["id"] == "boot-flash")
        flash["views"][0]["offset"] = "0x03f0_0000"

        self.assertDocumentFails(
            model,
            "firmware window 'boot-flash/reset vector view' overlaps node 0's 64 MiB memory",
        )

    def test_firmware_payload_view_must_fit_supplied_bytes(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin300.yaml"
        )
        firmware = model["machine"]["emulation"]["firmware_payload"]
        firmware["views"][0]["payload_offset"] = firmware["size"]

        self.assertDocumentFails(
            model,
            "firmware payload view 'CPU reset view' exceeds the supplied payload",
        )

    def test_firmware_second_selection_requires_address_and_size(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin300.yaml"
        )
        second_selection = model["machine"]["emulation"]["firmware_payload"][
            "second_selection"
        ]
        second_selection.pop("address")

        self.assertDocumentFails(model, "'address' is a required property")

    def test_reference_interrupt_omits_implementation_projection(self):
        model = self.fixture("reference-machine.yaml")
        model["interrupts"] = [{
            "id": "hardware-irq",
            "source": "xio-slot-11",
            "provenance": "vendor-document",
            "sources": ["Origin 2000 service manual"],
            "chain": [{"at": "hub", "as": "pending bit"}],
        }]

        self.assertDocumentPasses(model)

    def test_widget_and_xio_identity_roles(self) -> None:
        PASSES = object()

        def unchanged(model):
            pass

        def duplicate_enumerated_widget(model):
            model["connections"][1]["xio"]["decoded_widgets"] = [0]
            model["connections"][2]["xio"]["enumerated_widget"] = 8
            model["connections"][2]["xio"]["decoded_widgets"] = [9]

        def duplicate_discovery_widget(model):
            model["connections"][2]["xio"]["discovery_widget"] = 0
            model["connections"][2]["xio"]["decoded_widgets"] = [9]

        def duplicate_physical_port(model):
            model["connections"][1]["xio"]["physical_port"] = 9
            model["connections"][2]["xio"]["physical_port"] = 9
            model["connections"][2]["xio"]["decoded_widgets"] = [9]

        def decoded_widget_claims_enumerated_widget(model):
            model["connections"][1]["xio"]["decoded_widgets"] = [0]
            model["connections"][2]["xio"]["decoded_widgets"] = [8]
            model["connections"][2]["xio"]["enumerated_widget"] = 9
            model["connections"][1]["xio"]["decoded_widgets"].append(9)

        def legacy_widget_crosses_another_role(model):
            model["connections"][2]["xio"]["decoded_widgets"] = [9]
            model["connections"][2]["widget"] = 8

        def master_widget_crosses_another_identity(model):
            model["connections"][0]["xio"] = {"master_widget": 10}
            model["connections"][1]["xio"]["decoded_widgets"] = [10]

        def implicit_same_attachment_alias(model):
            identity = model["connections"][1]["xio"]
            identity["discovery_widget"] = 0
            identity["decoded_widgets"] = [0]

        def explicit_same_attachment_widget_alias(model):
            identity = model["connections"][1]["xio"]
            identity["discovery_widget"] = 0
            identity["decoded_widgets"] = [0]
            identity["identity_aliases"] = [
                {"widget": 0, "roles": ["discovery", "decoded"]}
            ]

        def explicit_same_attachment_legacy_alias(model):
            connection = model["connections"][1]
            connection["widget"] = 11
            connection["xio"]["enumerated_widget"] = 11
            connection["xio"]["identity_aliases"] = [
                {"widget": 11, "roles": ["legacy", "enumerated"]}
            ]

        def implicit_same_attachment_legacy_overlap(model):
            connection = model["connections"][1]
            connection["widget"] = 11
            connection["xio"]["enumerated_widget"] = 11

        def shared_master_widget(model):
            model["connections"][0]["xio"] = {"master_widget": 10}
            model["connections"][1]["xio"]["master_widget"] = 10

        cases = (
            ("conflicting decoded widget is rejected",
             "conflicting-decoded-widget.yaml", unchanged,
             "decoded widget 8"),
            ("duplicate enumerated widget is rejected",
             "conflicting-decoded-widget.yaml", duplicate_enumerated_widget,
             "enumerated widget 8"),
            ("duplicate discovery widget is rejected",
             "conflicting-decoded-widget.yaml", duplicate_discovery_widget,
             "discovery widget 0"),
            ("duplicate physical port is rejected",
             "conflicting-decoded-widget.yaml", duplicate_physical_port,
             "physical port 9"),
            ("decoded widget cannot claim another enumerated widget",
             "conflicting-decoded-widget.yaml",
             decoded_widget_claims_enumerated_widget,
             "widget 9 is enumerated by 'bridge-two'"),
            ("legacy widget cannot cross another identity role",
             "conflicting-decoded-widget.yaml",
             legacy_widget_crosses_another_role,
             "widget 8 has cross-role claims"),
            ("master widget cannot cross another component's identity",
             "reference-machine.yaml", master_widget_crosses_another_identity,
             "widget 10 has cross-role claims"),
            ("same-attachment widget aliases must be explicit",
             "reference-machine.yaml", implicit_same_attachment_alias,
             "widget 0 overlaps decoded and discovery roles"),
            ("explicit same-attachment widget alias is accepted",
             "reference-machine.yaml", explicit_same_attachment_widget_alias,
             PASSES),
            ("explicit same-attachment legacy alias is accepted",
             "reference-machine.yaml", explicit_same_attachment_legacy_alias,
             PASSES),
            ("same-attachment legacy overlap requires exact alias",
             "reference-machine.yaml", implicit_same_attachment_legacy_overlap,
             "widget 11 overlaps enumerated and legacy roles"),
            ("master widget may be shared as a master identity",
             "reference-machine.yaml", shared_master_widget, PASSES),
            ("widget identities require an XIO bus",
             "widget-identities-on-pci.yaml", unchanged,
             "only valid on an XIO bus"),
        )
        for name, fixture_name, mutate, expected in cases:
            with self.subTest(name):
                model = self.fixture(fixture_name)
                mutate(model)
                if expected is PASSES:
                    self.assertDocumentPasses(model)
                else:
                    self.assertDocumentFails(model, expected)

    def test_duplicate_component_id_is_rejected(self):
        self.assertCheckFails("duplicate-component.yaml", "duplicate component id 'hub'")

    def test_resolved_exclusion_rejects_appended_component(self):
        model = self.fixture("reference-machine.yaml")
        model["excluded_components"] = [{
            "match": {"id": "base-io-board"},
            "reason": "This expansion has no Origin 2000 BaseIO board.",
            "provenance": "vendor-document",
            "sources": ["Expansion topology fixture"],
        }]
        model["components"].append({
            "id": "base-io-board",
            "name": "BaseIO board",
            "kind": "board",
            "provenance": "vendor-document",
        })

        self.assertDocumentFails(
            model,
            "model excludes component 'base-io-board' matching id='base-io-board'",
        )

    def test_strict_exclusion_requires_evidence(self):
        model = self.fixture("reference-machine.yaml")
        model["excluded_components"] = [{
            "match": {"kind": "crossbar"},
            "reason": "The machine has no crossbar.",
        }]

        self.assertDocumentFails(
            model,
            "strict evidence policy requires provenance for excluded components selector kind='crossbar'",
        )

    def test_strict_exclusion_of_absent_component_is_valid(self):
        model = self.fixture("reference-machine.yaml")
        model["excluded_components"] = [{
            "match": {"kind": "crossbar"},
            "reason": "The machine has no crossbar.",
            "provenance": "vendor-document",
            "sources": ["Machine topology fixture"],
        }]

        self.assertDocumentPasses(model)

    def test_exclusion_selector_fields_match_conjunctively(self):
        model = self.fixture("reference-machine.yaml")
        model["excluded_components"] = [{
            "match": {"id": "xio-slot-11", "kind": "crossbar"},
            "reason": "Only a Crossbow at this ID would be excluded.",
            "provenance": "vendor-document",
            "sources": ["Machine topology fixture"],
        }]

        self.assertDocumentPasses(model)

    def test_containment_cycle_is_rejected(self):
        self.assertCheckFails("containment-cycle.yaml", "containment cycle")

    def test_node_asic_requires_processor_interface_facts(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        hub = next(c for c in model["components"] if c["id"] == "hub")
        hub.pop("node_asic", None)
        self.assertDocumentFails(model, "schema:")

    def test_node_asic_facts_describe_processor_interfaces(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        hub = next(c for c in model["components"] if c["id"] == "hub")
        self.assertEqual(
            hub["node_asic"],
            {"processor_interfaces": 1, "cpus_per_interface": 2},
        )
        self.assertDocumentPasses(model)

    def test_node_asic_facts_reject_unsupported_processor_interface_count(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        hub = next(c for c in model["components"] if c["id"] == "hub")
        hub["node_asic"]["processor_interfaces"] = 0
        self.assertDocumentFails(model, "schema:")

    def test_node_asic_facts_reject_unknown_field(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        hub = next(c for c in model["components"] if c["id"] == "hub")
        hub["node_asic"]["ports"] = 1
        self.assertDocumentFails(model, "schema:")

    def test_node_asic_facts_are_rejected_off_a_node_asic_component(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        cpu = next(c for c in model["components"] if c["id"] == "cpu0")
        cpu["node_asic"] = {"processor_interfaces": 1, "cpus_per_interface": 2}
        self.assertDocumentFails(model, "schema:")

    def test_bedrock_node_asic_carries_two_processor_interfaces(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin300.yaml")
        bedrock = next(c for c in model["components"] if c["id"] == "bedrock")
        self.assertEqual(
            bedrock["node_asic"],
            {"processor_interfaces": 2, "cpus_per_interface": 2},
        )
        self.assertDocumentPasses(model)

    def test_node_population_within_node_asic_interface_capacity_passes(self):
        model = self.fixture("node-asic-capacity.yaml")
        model["composition"]["population"]["cpus_per_node"] = [2]
        self.assertDocumentPasses(model)

    def test_node_population_exceeding_node_asic_interface_capacity_is_rejected(self):
        self.assertDocumentFails(
            self.fixture("node-asic-capacity.yaml"),
            "node 0 populates 3 CPUs, but node-asic 'hub' supports at most 2 "
            "(1 processor interface x 2 CPUs each)",
        )

    def test_node_asic_population_check_skips_when_counts_disagree(self):
        # population-orientation.yaml claims two nodes but authors only one
        # node-asic component; the check has no reliable per-node pairing and
        # must not guess one.
        self.assertDocumentPasses(self.fixture("population-orientation.yaml"))

    def test_crossbar_requires_a_declared_kind(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin2000.yaml")
        crossbow = next(c for c in model["components"] if c["id"] == "crossbow-0")
        crossbow.pop("crossbar", None)
        self.assertDocumentFails(model, "schema:")

    def test_crossbar_kind_matches_the_crossbow_hardware_kind(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin2000.yaml")
        crossbow = next(c for c in model["components"] if c["id"] == "crossbow-0")
        self.assertEqual(crossbow["crossbar"], {"kind": "xbow"})
        self.assertDocumentPasses(model)

    def test_crossbar_kind_matches_the_xbridge_hardware_kind(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin300.yaml")
        xbridge_crossbar = next(
            c for c in model["components"] if c["id"] == "xbridge-crossbar")
        self.assertEqual(xbridge_crossbar["crossbar"], {"kind": "xbridge"})
        self.assertDocumentPasses(model)

    def test_crossbar_rejects_unknown_kind(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin2000.yaml")
        crossbow = next(c for c in model["components"] if c["id"] == "crossbow-0")
        crossbow["crossbar"]["kind"] = "pic"
        self.assertDocumentFails(model, "schema:")

    def test_crossbar_facts_are_rejected_off_a_crossbar_component(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin2000.yaml")
        hub = next(c for c in model["components"] if c["id"] == "node-board-1-hub")
        hub["crossbar"] = {"kind": "xbow"}
        self.assertDocumentFails(model, "schema:")

    def test_pci_host_bridge_requires_a_head_count(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        bridge = next(c for c in model["components"] if c["id"] == "bridge")
        bridge.pop("pci_host_bridge", None)
        self.assertDocumentFails(model, "schema:")

    def test_bridge_carries_one_head(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        bridge = next(c for c in model["components"] if c["id"] == "bridge")
        self.assertEqual(bridge["pci_host_bridge"], {"heads": 1})
        self.assertDocumentPasses(model)

    def test_xbridge_carries_two_heads(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin300.yaml")
        xbridge = next(c for c in model["components"] if c["id"] == "xbridge")
        self.assertEqual(xbridge["pci_host_bridge"], {"heads": 2})
        self.assertDocumentPasses(model)

    def test_pci_host_bridge_rejects_zero_heads(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        bridge = next(c for c in model["components"] if c["id"] == "bridge")
        bridge["pci_host_bridge"]["heads"] = 0
        self.assertDocumentFails(model, "schema:")

    def test_pci_host_bridge_facts_are_rejected_off_other_kinds(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        hub = next(c for c in model["components"] if c["id"] == "hub")
        hub["pci_host_bridge"] = {"heads": 1}
        self.assertDocumentFails(model, "schema:")

    def test_system_controller_requires_a_declared_kind(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        elsc = next(c for c in model["components"] if c["id"] == "sgi-elsc")
        elsc.pop("system_controller", None)
        self.assertDocumentFails(model, "schema:")

    def test_origin_200_elsc_is_the_msc_system_controller(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        elsc = next(c for c in model["components"] if c["id"] == "sgi-elsc")
        self.assertEqual(elsc["system_controller"], {"kind": "msc"})
        self.assertDocumentPasses(model)

    def test_origin_300_l1_is_the_l1_system_controller(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin300.yaml")
        l1 = next(c for c in model["components"] if c["id"] == "l1")
        self.assertEqual(l1["system_controller"], {"kind": "l1"})
        self.assertDocumentPasses(model)

    def test_system_controller_rejects_unknown_kind(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        elsc = next(c for c in model["components"] if c["id"] == "sgi-elsc")
        elsc["system_controller"]["kind"] = "elsc"
        self.assertDocumentFails(model, "schema:")

    def test_system_controller_facts_are_rejected_off_other_kinds(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        hub = next(c for c in model["components"] if c["id"] == "hub")
        hub["system_controller"] = {"kind": "msc"}
        self.assertDocumentFails(model, "schema:")

    def test_memory_block_is_optional_on_a_memory_component(self):
        # node-board-1-directory-memory is kind memory but carries no
        # dimm-slot geometry of its own; the block is populated only where
        # a source states it.
        model = self.checker_module.load_system_model(
            INPUTS / "origin2000.yaml")
        directory_memory = next(
            c for c in model["components"] if c["id"] == "node-board-1-directory-memory")
        self.assertNotIn("memory", directory_memory)
        self.assertDocumentPasses(model)

    def test_origin_200_node_memory_carries_its_bank_geometry(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        node_memory = next(
            c for c in model["components"] if c["id"] == "node-memory")
        self.assertEqual(
            node_memory["memory"],
            {
                "dimm_slots": 8,
                "banks": 4,
                "dimms_per_bank": 2,
                "bank_sizes_mib": [32, 64, 128, 512],
            },
        )
        self.assertDocumentPasses(model)

    def test_origin_2000_main_memory_carries_directory_dimm_slots(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin2000.yaml")
        main_memory = next(
            c for c in model["components"] if c["id"] == "node-board-1-main-memory")
        self.assertEqual(
            main_memory["memory"],
            {
                "dimm_slots": 32,
                "directory_dimm_slots": 8,
                "banks": 8,
                "dimms_per_bank": 2,
                "bank_sizes_mib": [64, 128, 512],
            },
        )
        self.assertDocumentPasses(model)

    def test_memory_block_rejects_unknown_field(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        node_memory = next(
            c for c in model["components"] if c["id"] == "node-memory")
        node_memory["memory"]["rows"] = 2
        self.assertDocumentFails(model, "schema:")

    def test_memory_block_is_rejected_off_a_non_memory_component(self):
        model = self.checker_module.load_system_model(
            INPUTS / "origin200.yaml")
        hub = next(c for c in model["components"] if c["id"] == "hub")
        hub["memory"] = {"banks": 4}
        self.assertDocumentFails(model, "schema:")

    def test_memory_population_within_declared_bank_capacity_passes(self):
        model = self.fixture("memory-bank-capacity.yaml")
        model["composition"]["population"]["memory_bank_mib_per_node"] = [
            [64, 64, 0]
        ]
        self.assertDocumentPasses(model)

    def test_memory_population_exceeding_declared_bank_capacity_is_rejected(self):
        self.assertDocumentFails(
            self.fixture("memory-bank-capacity.yaml"),
            "node 0 populates 3 memory banks, but memory 'node-memory' "
            "supports at most 2",
        )

    def test_memory_bank_check_skips_when_counts_disagree(self):
        # population-orientation.yaml claims two nodes but authors only one
        # memory component with no memory block at all.
        self.assertDocumentPasses(self.fixture("population-orientation.yaml"))


if __name__ == "__main__":
    unittest.main()
