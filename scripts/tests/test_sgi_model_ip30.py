# SPDX-License-Identifier: BSD-3-Clause
"""IP30 catalogue records and operator-selected R12000 inputs."""

import copy
import json
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from sgi_model.model_compile import CompileError, compile_model
from sgi_model.model_qemu import render_qemu_inputs
from sgi_model.model_validation import load_checked_system_model, schema

MODELS = ROOT / "hw/mips/sgi/models"


def resolved(name):
    return load_checked_system_model(MODELS / f"{name}.yaml",
                                     model_schema=schema(MODELS))


class IP30CatalogueTests(unittest.TestCase):
    def test_octane_has_heart_and_existing_population(self):
        topology = compile_model(resolved("octane"))
        node = topology["nodes"][0]
        self.assertEqual(topology["generation"], "IP30")
        self.assertEqual(node["asic_kind"], "HEART")
        self.assertNotIn("identity", node)
        self.assertEqual(node["bank_mib"], [128, 0, 0, 0])
        self.assertEqual(node["cpu_slots"][0]["assembly"]["prid"], 0x927)

    def test_octane2_keeps_all_operator_cpu_inputs(self):
        catalogue = json.loads(render_qemu_inputs(MODELS)[
            "hw/mips/sgi/sn-catalogue.json"])
        topology = next(t for t in catalogue["topologies"] if t["name"] == "octane2")
        cpu = topology["nodes"][0]["cpu-slots"][0]["assembly"]
        self.assertNotIn("prid", cpu)
        self.assertNotIn("fpu-prid", cpu)
        self.assertNotIn("mode-bits", cpu)
        self.assertEqual(cpu["required-properties"], [
            "r12000-prid", "r12000-fpu-id", "r12000-reset-mode",
            "r12000-scache-bytes", "r12000-scache-block-words"])
        offering = next(o for o in catalogue["offerings"] if o["product"] == "octane2")
        self.assertNotIn("asset", offering["firmware"])
        self.assertEqual(offering["firmware"]["kind"], "ip30-prom")
        self.assertEqual(len(offering["storage"]), 2)
        self.assertEqual(len(offering["consoles"]), 2)
        self.assertEqual(len(offering["networks"]), 1)


    @staticmethod
    def component(model, kind):
        return next(c for c in model["components"]
                    if c.get("hardware_kind") == kind)

    def test_missing_heart_identity_is_rejected(self):
        model = resolved("octane")
        del self.component(model, "heart")["identity"]
        with self.assertRaisesRegex(CompileError, "has no identity"):
            compile_model(model)

    def test_heart_widget_revision_must_match_id(self):
        model = resolved("octane")
        self.component(model, "heart")["identity"]["widget_id_64"] = "0x6c00106d"
        with self.assertRaisesRegex(CompileError, "disagrees with its revision"):
            compile_model(model)

    def test_all_board_roles_and_distinct_rom_serials_are_required(self):
        source = resolved("octane")
        for fault, message in (("missing", "requires processor"),
                               ("role", "repeated IP30 board record"),
                               ("serial", "serials must be distinct"),
                               ("width", "48-bit")):
            with self.subTest(fault=fault):
                model = copy.deepcopy(source)
                records = [c for c in model["components"] if "board_record" in c]
                if fault == "missing":
                    del records[-1]["board_record"]
                elif fault == "role":
                    records[-1]["board_record"]["role"] = records[0]["board_record"]["role"]
                elif fault == "serial":
                    records[-1]["board_record"]["serial"] = records[0]["board_record"]["serial"]
                else:
                    records[-1]["board_record"]["serial"] = 1 << 48
                with self.assertRaisesRegex(CompileError, message):
                    compile_model(model)

    def test_r12000_profile_declarations_are_required_and_do_not_supply_defaults(self):
        source = resolved("octane2")
        for fault in ("missing", "default"):
            with self.subTest(fault=fault):
                model = copy.deepcopy(source)
                identity = self.component(model, "r12000")["identity"]
                if fault == "missing":
                    identity["required_properties"].pop()
                else:
                    identity["processor_identification"] = "0xe35"
                with self.assertRaisesRegex(CompileError, "explicit profile|operator inputs"):
                    compile_model(model)

    def test_r12000_property_declaration_order_is_not_significant(self):
        model = resolved("octane2")
        identity = self.component(model, "r12000")["identity"]
        identity["required_properties"].reverse()
        self.assertEqual(set(compile_model(model)["nodes"][0]["cpu_slots"][0]
                             ["assembly"]["required_properties"]),
                         set(identity["required_properties"]))

    def test_cpu_population_stays_single_processor(self):
        model = resolved("octane")
        model["composition"]["population"]["cpus_per_node"] = [2]
        with self.assertRaisesRegex(CompileError, "single-processor population"):
            compile_model(model)

    def test_memory_layouts_match_total_capacity_and_sequential_banks(self):
        source = resolved("octane")
        for row, message in (([64, 0, 64, 0], "sequentially"),
                             ([32, 32, 32, 32], "64, 128 or 256"),
                             ([64, 64, 64, 0], "selected total")):
            with self.subTest(row=row):
                model = copy.deepcopy(source)
                model["machine"]["emulation"]["emulator_constraints"]                    ["accepted_bank_layouts_mib"].append(row)
                with self.assertRaisesRegex(CompileError, message):
                    compile_model(model)


    def test_emitted_layouts_always_put_the_default_first(self):
        model = resolved("octane")
        model["machine"]["emulation"]["emulator_constraints"]            ["accepted_bank_layouts_mib"].reverse()
        node = compile_model(model)["nodes"][0]
        self.assertEqual(node["accepted_bank_layouts"][0]["bank_mib"],
                         node["bank_mib"])


if __name__ == "__main__":
    unittest.main()
