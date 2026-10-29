# SPDX-License-Identifier: BSD-3-Clause
"""The fixed O2 product carries no Scalable Node wiring."""
import copy
import json
import pathlib
import sys
import unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from sgi_model.model_compile import CompileError, compile_model
from sgi_model.model_qemu import render_qemu_inputs
from sgi_model.model_validation import load_checked_system_model, schema
MODELS = pathlib.Path(__file__).resolve().parents[2] / "hw/mips/sgi/models"

class IP32CatalogueTests(unittest.TestCase):
    def model(self):
        return load_checked_system_model(MODELS / "o2.yaml", model_schema=schema(MODELS))

    def test_fixed_product(self):
        topology = compile_model(self.model())
        self.assertEqual(topology["generation"], "IP32")
        self.assertEqual(topology["node_populations"][0]["grade"], "INFERRED")
        node = topology["nodes"][0]
        self.assertEqual(node["asic_kind"], "CRIME")
        self.assertEqual(node["bank_mib"], [128] * 8)
        self.assertEqual(node["accepted_memory_mib"], [1024])
        self.assertEqual(node["cpu_slots"][0]["assembly"], {
            "id": "cpu0", "kind": "R10000", "clock_hz": 195000000,
            "cache_mib": 1, "mode_bits": 0x00198603})
        for field in ("routers", "links", "crossbars", "xio_attachments", "xio_slots"):
            self.assertEqual(topology[field], [])

    def test_offering(self):
        catalogue = json.loads(render_qemu_inputs(MODELS)["hw/mips/sgi/sgi-machines.json"])
        topology = next(t for t in catalogue["topologies"] if t["product"] == "o2")
        self.assertNotIn("module-index", topology["nodes"][0])
        self.assertNotIn("slot", topology["nodes"][0])
        offering = next(o for o in catalogue["offerings"] if o["product"] == "o2")
        self.assertEqual(offering["machine-options"], "o2")
        self.assertEqual(offering["launch-recipe"], "ip32")
        self.assertEqual(offering["console"], "mace-serial-a")
        self.assertEqual(offering["storage"], [])
        self.assertEqual(offering["firmware"]["size"], 524288)
        self.assertNotIn("asset", offering["firmware"])
        self.assertEqual([c["launcher-name"] for c in offering["consoles"]], ["mace-a", "mace-b"])
        self.assertEqual([a["bus"] for a in offering["scsi-adapters"]],
                         ["aic0.0", "aic1.0"])
        self.assertEqual(offering["scsi-adapters"][1]["targets"], [])
        self.assertEqual(offering["disk-bus"], "aic0.0")
        self.assertEqual(offering["overrides"], [])
        self.assertEqual(offering["networks"][0]["nic-model"], "sgi-mace-eth")

    def test_o2plus_is_a_cpu_variant_of_the_same_machine(self):
        model = load_checked_system_model(MODELS / "o2plus.yaml",
                                          model_schema=schema(MODELS))
        topology = compile_model(model)
        cpu = topology["nodes"][0]["cpu_slots"][0]["assembly"]
        self.assertEqual(cpu["kind"], "R12000")
        self.assertEqual(cpu["clock_hz"], 400000000)
        self.assertEqual(cpu["cache_mib"], 2)
        self.assertEqual(cpu["prid"], 0x0e35)
        self.assertEqual(cpu["fpu_prid"], 0x0900)
        self.assertEqual(cpu["mode_bits"], 0x011a8603)
        self.assertEqual(cpu["required_properties"], [
            "r12000-prid", "r12000-fpu-id", "r12000-reset-mode",
            "r12000-scache-bytes", "r12000-scache-block-words"])
        catalogue = json.loads(render_qemu_inputs(MODELS)[
            "hw/mips/sgi/sgi-machines.json"])
        offering = next(o for o in catalogue["offerings"]
                        if o["topology"] == "o2plus")
        self.assertEqual(offering["product"], "o2")
        self.assertEqual(offering["machine-options"], "o2,variant=o2plus")
        self.assertEqual(offering["default-cpu-model"], "R12000")
        self.assertEqual(offering["disk-bus"], "aic0.0")
        self.assertEqual(offering["default-topology"], "o2")
        self.assertEqual(offering["memory-per-node-mib"], {
            "default": 1024, "accepted": [1024]})
        baseline = next(o for o in catalogue["offerings"]
                        if o["topology"] == "o2")
        for field in ("console", "consoles", "storage", "firmware",
                      "resources", "scsi-adapters", "networks", "launch-recipe"):
            self.assertEqual(offering[field], baseline[field])
        cpu_model = next(c for c in model["components"] if c["id"] == "cpu0")
        cpu_model["identity"]["clock_mhz"] = 195
        with self.assertRaises(CompileError):
            compile_model(model)

    def test_fixed_inventory_is_required(self):
        for identifier in ("mace", "gbe", "aic0", "aic1", "flash", "phy"):
            model = self.model()
            model["components"] = [c for c in model["components"]
                                   if c["id"] != identifier]
            with self.subTest(component=identifier), self.assertRaises(CompileError):
                compile_model(model)

    def test_unqualified_populations_are_rejected(self):
        for field, value in (("cpus_per_node", [2]), ("memory_mib_per_node", [512]),
                             ("memory_bank_mib_per_node", [[128] * 4])):
            model = copy.deepcopy(self.model())
            model["composition"]["population"][field] = value
            with self.assertRaises(CompileError):
                compile_model(model)
        model = self.model()
        next(c for c in model["components"] if c["id"] == "cpu0")["hardware_kind"] = "r12000"
        with self.assertRaises(CompileError):
            compile_model(model)

if __name__ == "__main__":
    unittest.main()
