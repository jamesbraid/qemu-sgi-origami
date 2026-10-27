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

    def test_every_registry_entry_has_a_label(self):
        registry = load_registry()
        page = references_doc.OUTPUT.read_text()
        for group, _title, _intro in references_doc.GROUPS:
            for key in registry.get(group) or {}:
                with self.subTest(group=group, key=key):
                    self.assertIn(f".. _{references_doc.label(group, key)}:", page)


if __name__ == "__main__":
    unittest.main()
