# SPDX-License-Identifier: BSD-3-Clause

#!/usr/bin/env python3
"""Keep the generated SGI settings page in step with the catalogue."""

import json
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from sgi_model import settings_doc
from sgi_model.model_qemu import render_qemu_inputs


class SettingsPageTests(unittest.TestCase):
    def test_checked_in_page_matches_the_catalogue(self):
        self.assertEqual(
            settings_doc.OUTPUT.read_text(),
            settings_doc.render(json.loads(
                settings_doc.CATALOGUE.read_text())),
            "run scripts/sgi_model/settings_doc.py to regenerate the page")

    def test_checked_in_catalogue_is_the_generated_one(self):
        generated = render_qemu_inputs(ROOT / "hw/mips/sgi/models")
        self.assertEqual(settings_doc.CATALOGUE.read_text(),
                         generated["hw/mips/sgi/sgi-machines.json"])

    def test_every_product_has_a_table(self):
        catalogue = json.loads(settings_doc.CATALOGUE.read_text())
        page = settings_doc.OUTPUT.read_text()
        for product in {o["product"] for o in catalogue["offerings"]}:
            with self.subTest(product=product):
                self.assertIn(f"``{product}``\n", page)

    def test_definitions_name_real_kinds_and_forms(self):
        catalogue = json.loads(settings_doc.CATALOGUE.read_text())
        names = [d["name"] for d in catalogue["setting-definitions"]]
        self.assertEqual(len(names), len(set(names)))
        for d in catalogue["setting-definitions"]:
            with self.subTest(setting=d["name"]):
                self.assertRegex(d["name"], r"^[a-z][a-z0-9-]*$")
                if d["kind"] == "enum":
                    self.assertTrue(d["values"])
                if d["target"]["kind"] == "global":
                    self.assertIn("device", d["target"])


if __name__ == "__main__":
    unittest.main()
