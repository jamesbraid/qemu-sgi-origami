# SPDX-License-Identifier: BSD-3-Clause
"""IP30 catalogue records and the catalogued R12000 profile."""

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

    def test_octane2_carries_its_r12000_profile(self):
        catalogue = json.loads(render_qemu_inputs(MODELS)[
            "hw/mips/sgi/sgi-machines.json"])
        topology = next(t for t in catalogue["topologies"] if t["name"] == "octane2")
        cpu = topology["nodes"][0]["cpu-slots"][0]["assembly"]
        self.assertEqual(cpu["prid"], 0x0e35)
        self.assertEqual(cpu["fpu-prid"], 0x0900)
        self.assertEqual(cpu["mode-bits"], 0x001aa683)
        self.assertEqual(cpu["cache-mib"], 2)
        self.assertNotIn("required-properties", cpu)
        offering = next(o for o in catalogue["offerings"] if o["product"] == "octane2")
        self.assertNotIn("required-properties", offering["processor"])
        self.assertEqual(offering["machine-options"],
                         "octane2,topology=octane2,nodes=1")
        self.assertNotIn("firmware", offering)
        self.assertEqual(offering["init-inputs"], [
            {"name": "boot-prom", "kind": "ip30-prom", "required": True}])
        self.assertEqual(offering["storage"], [
            {"name": "node0-flash", "size": 1048576, "read-only": False,
             "initial": "boot-prom"},
            {"name": "nvram0", "size": 512, "read-only": False,
             "initial": "zero"},
            {"name": "nvram0-clock", "size": 24, "read-only": False,
             "initial": "zero"}])
        # The topology's flash is the same 1 MiB part the store backs.
        topology = next(t for t in catalogue["topologies"]
                        if t["name"] == "octane2")
        self.assertEqual(topology["nodes"][0]["flash"],
                         {"kind": "operator-prom", "size": 1048576,
                          "select": False, "strap": False})
        # Board values with catalogue defaults are machine properties; the
        # processor part's are -cpu properties.
        self.assertEqual(offering["overrides"], [
            {"property": "flash-select", "target": "machine"},
            {"property": "flash-strap", "target": "machine"},
            {"property": "mac", "target": "machine"},
            {"property": "r12000-prid", "target": "cpu"},
            {"property": "r12000-fpu-id", "target": "cpu"},
            {"property": "r12000-reset-mode", "target": "cpu"},
            {"property": "r12000-scache-bytes", "target": "cpu"},
            {"property": "r12000-scache-block-words", "target": "cpu"}])
        self.assertEqual(len(offering["scsi-adapters"]), 2)
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

    def test_ds1685_is_the_io_board_timekeeper(self):
        """The Octane NVRAM stores follow from the IP30 board's DS1685
        record, as the SN stores follow from their Timekeepers."""
        topology = compile_model(resolved("octane"))
        self.assertEqual(topology["io_boards"][0]["system_timekeeper"],
                         {"kind": "ds1685", "initial_ip27_fixture": False})

    def test_ds1685_policy_is_checked(self):
        from sgi_model import model_composition
        from sgi_model.model_composition import ResolutionError

        source = resolved("octane")
        for fault in ("kind", "year_base"):
            with self.subTest(fault=fault):
                model = copy.deepcopy(source)
                policy = model["machine"]["emulation"][
                    "system_timekeepers"]["ip30-board"]
                if fault == "kind":
                    policy["kind"] = "m48t35-compatible"
                else:
                    policy["year_base"] = 1968
                with self.assertRaisesRegex(ResolutionError,
                                            "unsupported system Timekeeper"):
                    model_composition.validate_system_timekeepers(model)

    def test_boot_flash_wiring_is_required(self):
        model = resolved("octane")
        del model["machine"]["emulation"]["boot_flash_wiring"]
        with self.assertRaisesRegex(CompileError, "boot_flash_wiring"):
            compile_model(model)

    def test_boot_flash_wiring_reaches_the_topology(self):
        model = resolved("octane")
        model["machine"]["emulation"]["boot_flash_wiring"]["strap"] = True
        flash = compile_model(model)["nodes"][0]["flash"]
        self.assertEqual((flash["select"], flash["strap"]), (False, True))

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

    @staticmethod
    def r12000_variant(model):
        entry = next(p for p in model["processors"] if p["part"] == "r12000")
        return entry["variants"][0]

    def test_r12000_profile_values_are_required(self):
        source = resolved("octane2")
        for field in ("processor_identification", "fpu_identification",
                      "reset_mode_bits"):
            with self.subTest(field=field):
                model = copy.deepcopy(source)
                if field == "reset_mode_bits":
                    del self.r12000_variant(model)[field]
                else:
                    del self.component(model, "r12000")["identity"][field]
                with self.assertRaisesRegex(CompileError, field):
                    compile_model(model)

    def test_r12000_profile_values_are_checked(self):
        source = resolved("octane2")
        for fault, message in (
                ("prid", "implementation 0x0e"),
                ("wide", "25 bits"),
                ("little", "big-endian"),
                ("cache", "SS disagrees")):
            with self.subTest(fault=fault):
                model = copy.deepcopy(source)
                variant = self.r12000_variant(model)
                mode = int(variant["reset_mode_bits"], 0)
                if fault == "prid":
                    self.component(model, "r12000")["identity"][
                        "processor_identification"] = "0x0935"
                elif fault == "wide":
                    variant["reset_mode_bits"] = hex(mode | 1 << 25)
                elif fault == "little":
                    variant["reset_mode_bits"] = hex(mode & ~(1 << 15))
                else:
                    variant["reset_mode_bits"] = hex(mode & ~(7 << 16)
                                                     | 1 << 16)
                with self.assertRaisesRegex(CompileError, message):
                    compile_model(model)

    def test_cpu_population_stays_single_processor(self):
        model = resolved("octane")
        model["composition"]["population"]["cpus_per_node"] = [2]
        with self.assertRaisesRegex(CompileError, "single-processor population"):
            compile_model(model)

    def test_bank_rules_follow_the_owners_guides(self):
        # Octane: 64 to 512 MiB a bank in any order, up to 2 GiB. Octane2:
        # 64 MiB to 2 GiB a bank, largest first, up to 8 GiB.
        for name, sizes, descending, top in (
                ("octane", [64, 128, 256, 512], False, 2048),
                ("octane2", [64, 128, 256, 512, 1024, 2048], True, 8192)):
            with self.subTest(product=name):
                node = compile_model(resolved(name))["nodes"][0]
                self.assertEqual(node["accepted_bank_mib"], sizes)
                self.assertEqual(node["banks_descending"], descending)
                totals = node["accepted_memory_mib"]
                self.assertEqual((totals[0], totals[-1]), (64, top))
                self.assertIn(sum(node["bank_mib"]), totals)

    def test_accepted_totals_are_what_four_banks_make(self):
        node = compile_model(resolved("octane2"))["nodes"][0]
        totals = node["accepted_memory_mib"]
        # 3072 is 2048 + 1024; 4800 needs five banks; 8256 is past 8 GiB.
        self.assertIn(3072, totals)
        self.assertNotIn(4800, totals)
        self.assertNotIn(8256, totals)
        self.assertNotIn(96, totals)
        self.assertEqual(len(totals), 79)
        self.assertEqual(len(compile_model(resolved("octane"))["nodes"][0]
                             ["accepted_memory_mib"]), 27)

    def test_bank_sizes_must_be_heart_pairs(self):
        for size in (32, 96, 4096):
            with self.subTest(size=size):
                model = resolved("octane")
                model["machine"]["emulation"]["emulator_constraints"][
                    "accepted_bank_sizes_mib"].append(size)
                with self.assertRaisesRegex(CompileError, "pair making 64"):
                    compile_model(model)

    def test_default_population_must_follow_the_bank_rules(self):
        model = resolved("octane2")
        model["machine"]["emulation"]["emulator_constraints"][
            "accepted_bank_sizes_mib"] = [64, 128]
        with self.assertRaisesRegex(CompileError, "default bank population"):
            compile_model(model)

    def test_descending_order_refuses_a_smaller_first_bank(self):
        model = resolved("octane2")
        model["composition"]["population"]["memory_bank_mib_per_node"] = [
            [128, 256, 0, 0]]
        model["composition"]["population"]["memory_mib_per_node"] = [384]
        with self.assertRaisesRegex(CompileError, "default bank population"):
            compile_model(model)


if __name__ == "__main__":
    unittest.main()
