#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Prove that a :src: citation of an id the registry lacks fails the
manual's build, and that a registered one passes.

The pages are built as CI builds them (Sphinx with warnings as errors)
from a small tree: the real generated references page and the real
citation role, plus one page that cites ids.
"""

import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from sgi_model.citations import load_registry
from sgi_model import references_doc

CONF = f"""\
import sys
sys.path.insert(0, {str(ROOT / 'docs/sphinx')!r})
project = 'citation check'
extensions = ['sgiref']
master_doc = 'index'
exclude_patterns = ['_build']
"""

INDEX = """\
Citation check
==============

.. toctree::

   references
   page

"""


def registered_id():
    registry = load_registry()
    for group, _title, _intro in references_doc.GROUPS:
        for key in registry.get(group) or {}:
            return key
    raise AssertionError("the registry lists no source")


class SrcRoleBuildTests(unittest.TestCase):
    def build(self, body):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / "src"
            src.mkdir()
            (src / "conf.py").write_text(CONF)
            (src / "index.rst").write_text(INDEX)
            (src / "references.rst").write_text(
                references_doc.render(load_registry()))
            (src / "page.rst").write_text(
                "Page\n====\n\n" + body + "\n")
            done = subprocess.run(
                [sys.executable, "-m", "sphinx", "-W", "--keep-going", "-q",
                 "-b", "html", "-d", str(pathlib.Path(tmp) / "doctrees"),
                 str(src), str(pathlib.Path(tmp) / "html")],
                capture_output=True, text=True)
            return done.returncode, done.stdout + done.stderr

    def test_registered_id_builds(self):
        status, out = self.build(
            f"A fact (:src:`{registered_id()} p. 4`).")
        self.assertEqual(status, 0, out)

    def test_wrapped_locator_builds(self):
        status, out = self.build(
            f"A fact (:src:`{registered_id()}\np. 4`).")
        self.assertEqual(status, 0, out)

    def test_unknown_id_fails_the_build(self):
        status, out = self.build(
            "A fact (:src:`no-such-source-id p. 4`).")
        self.assertNotEqual(status, 0, out)
        self.assertIn("sgi-ref-no-such-source-id", out)

    def test_unknown_id_without_a_locator_fails_the_build(self):
        status, _out = self.build("A fact (:src:`no-such-source-id`).")
        self.assertNotEqual(status, 0)

    def test_a_misspelt_registered_id_fails_the_build(self):
        status, _out = self.build(
            f"A fact (:src:`{registered_id()}x p. 4`).")
        self.assertNotEqual(status, 0)


if __name__ == "__main__":
    unittest.main()
