# SPDX-License-Identifier: BSD-3-Clause

#!/usr/bin/env python3
"""Keep the generated SGI references page in step with the registry."""

import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from sgi_model import references_doc
from sgi_model.citations import load_registry


class ReferencesPageTests(unittest.TestCase):
    def test_checked_in_page_matches_the_registry(self):
        self.assertEqual(
            references_doc.OUTPUT.read_text(),
            references_doc.render(load_registry()),
            "run scripts/sgi_model/references_doc.py to regenerate the page")

    def test_labels_are_unique_across_groups(self):
        registry = load_registry()
        labels = [references_doc.label(key)
                  for group, _title, _intro in references_doc.GROUPS
                  for key in registry.get(group) or {}]
        self.assertEqual(len(labels), len(set(labels)))

    def test_sphinx_role_derives_the_same_label(self):
        sys.path.insert(0, str(ROOT / "docs/sphinx"))
        try:
            import sgiref
        except ImportError as error:
            self.skipTest(f"Sphinx is not installed: {error}")
        for key in ("007-3439-002", "ip27-prom-6.156", "lore-kaloz-ip35-35"):
            self.assertEqual(sgiref.label(key), references_doc.label(key))

    def test_every_registry_entry_has_a_label(self):
        registry = load_registry()
        page = references_doc.OUTPUT.read_text()
        for group, _title, _intro in references_doc.GROUPS:
            for key in registry.get(group) or {}:
                with self.subTest(group=group, key=key):
                    self.assertIn(f".. _{references_doc.label(key)}:", page)


if __name__ == "__main__":
    unittest.main()
