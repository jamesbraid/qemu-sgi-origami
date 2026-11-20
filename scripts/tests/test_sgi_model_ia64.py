# SPDX-License-Identifier: BSD-3-Clause
"""The fixed Altix 350 product runs on qemu-system-ia64, not qemu-system-mips64."""
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


class IA64CatalogueTests(unittest.TestCase):
    def model(self):
        return load_checked_system_model(MODELS / "altix350.yaml",
                                         model_schema=schema(MODELS))

    def catalogue(self):
        return json.loads(render_qemu_inputs(MODELS)["hw/mips/sgi/sgi-machines.json"])

    def test_fixed_product(self):
        topology = compile_model(self.model())
        self.assertEqual(topology["generation"], "IA64")
        node = topology["nodes"][0]
        self.assertEqual(node["asic_kind"], "SHUB")
        self.assertEqual(len(node["cpu_slots"]), 2)
        self.assertEqual(node["accepted_memory_mib"], [2048])
        self.assertEqual(node["flash"]["size"], 6291456)

    def test_offerings(self):
        catalogue = self.catalogue()
        offerings = [o for o in catalogue["offerings"] if o["product"] == "altix350"]
        self.assertEqual([(o["id"], o["smp"]) for o in offerings],
                         [("altix350.1", 1), ("altix350.2", 2)])
        for offering in offerings:
            self.assertEqual(offering["target"], "ia64")
            self.assertEqual(offering["machine-options"], "altix350")
            self.assertEqual(offering["prom-dialogue"], "efi-shell")
            self.assertEqual(offering["default-cpu-model"], "madison-1600-3m")
            self.assertEqual(offering["memory-per-node-mib"],
                             {"default": 2048, "accepted": [2048]})
            self.assertEqual(offering["clock"], {
                "icount-shifts": [], "icount-off": True, "rtc": ["host"],
                "default-rtc": "host", "vm-needs-icount": False})
            for field in ("init-inputs", "storage", "resources", "settings",
                          "setting-groups"):
                self.assertEqual(offering[field], [])
            self.assertEqual(offering["firmware"]["argument"], "-bios")
            self.assertEqual(offering["firmware"]["size"], 6291456)
            self.assertEqual(
                offering["firmware"]["sha256"],
                "51ccf9ad830b85ee6c88d6d73d2512322a86454e63db51c57454679283a34617")
            self.assertEqual([a["bus"] for a in offering["scsi-adapters"]],
                             ["scsi.0", "scsi.1"])
            self.assertEqual(offering["scsi-adapters"][0]["targets"],
                             list(range(1, 16)))
            self.assertEqual(offering["scsi-adapters"][0]["boot-target"], 1)
            self.assertEqual(offering["scsi-adapters"][1]["targets"], [])
            self.assertEqual(offering["serial-lines"], [
                {"name": "l1", "chardev": "l1", "kind": "console",
                 "node": 0, "serial": 0}])
            self.assertEqual(offering["consoles"], [
                {"kind": "console", "node": 0, "argument": "-serial chardev:l1",
                 "launcher-name": "l1"}])
            self.assertEqual(offering["hostid-source"], "address")
            self.assertEqual(offering["networks"][0]["nic-model"], "bcm570x")
            self.assertEqual(offering["networks"][0]["mac"], {
                "derived": True, "machine-property": "mac", "offset": 0,
                "default-address": "08:00:69:e1:00:00",
                "default-provenance": "approximated"})

    def test_fixed_inventory_and_population_are_required(self):
        for identifier in ("shub", "pic", "scsi", "ethernet", "cpu1"):
            model = self.model()
            model["components"] = [c for c in model["components"]
                                   if c["id"] != identifier]
            with self.subTest(component=identifier), self.assertRaises(CompileError):
                compile_model(model)
        model = copy.deepcopy(self.model())
        model["composition"]["population"]["memory_mib_per_node"] = [1024]
        with self.assertRaises(CompileError):
            compile_model(model)


if __name__ == "__main__":
    unittest.main()
