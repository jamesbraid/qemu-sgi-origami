# SPDX-License-Identifier: BSD-3-Clause

#!/usr/bin/env python3
"""Tests for the model-citation check and the reference registry."""

import copy
import pathlib
import shutil
import sys
import tempfile
import textwrap
import unittest

import jsonschema
import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
INPUTS = ROOT / "hw/mips/sgi/models"
sys.path.insert(0, str(ROOT / "scripts"))

from sgi_model import citations


def classes(text, registry, tree=ROOT):
    return [cls for cls, _ in citations.classify(text, registry, tree)[1]]


class RegistryTests(unittest.TestCase):
    def test_registry_matches_its_schema(self):
        registry = citations.load_registry(INPUTS)
        self.assertIn("007-3708-002", registry["documents"])
        self.assertEqual(registry["trees"]["openbsd-sgi"]["branch"],
                         "sgi-is-alive-at-7.9")

    def test_tree_without_a_location_must_say_so(self):
        registry = citations.load_registry(INPUTS)
        schema = yaml.safe_load((INPUTS / citations.REGISTRY_SCHEMA).read_text())
        validator = jsonschema.Draft202012Validator(schema)
        broken = copy.deepcopy(registry)
        broken["trees"]["linux-ip35"] = {"title": "t", "url": None}
        self.assertTrue(list(validator.iter_errors(broken)))
        broken["trees"]["linux-ip35"] = {
            "title": "t", "url": "https://example.org/x.git",
            "public_location": "unknown", "note": "n"}
        self.assertTrue(list(validator.iter_errors(broken)))

    def test_firmware_entries_name_version_and_hash(self):
        registry = citations.load_registry(INPUTS)
        for key, entry in registry["firmware"].items():
            with self.subTest(key=key):
                self.assertEqual(entry["kind"], "firmware")
                self.assertRegex(entry["obtain"]["sha256"], r"^[0-9a-f]{64}$")
                self.assertIn(entry["version"], key)

    def test_registry_names_no_other_repository(self):
        text = (INPUTS / citations.REGISTRY).read_text()
        self.assertEqual(citations.internal_pointers(text), [])
        self.assertNotIn("docs/library", text)


class ClassifyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.registry = citations.load_registry(INPUTS)

    def test_document_citations(self):
        self.assertEqual(classes("007-3708-002 Table 1-1 printed page 5", self.registry), [])
        self.assertEqual(classes("007-3708-002", self.registry), ["missing-locator"])
        self.assertEqual(classes("007-9999-001 page 3", self.registry), ["unregistered"])

    def test_web_and_firmware_citations(self):
        self.assertEqual(classes("web:dmesgd-8994 the onewire0 line", self.registry), [])
        self.assertEqual(classes("web:nekonomicon-16727716 post 2", self.registry),
                         ["obtain-only"])
        self.assertEqual(classes("web:no-such-key page 1", self.registry), ["unregistered"])
        self.assertEqual(classes("web:Bad_Key page 1", self.registry), ["malformed"])
        self.assertEqual(classes("firmware:ip27-prom-6.156 string \"x\"", self.registry),
                         ["firmware", "obtain-only"])
        self.assertEqual(classes("firmware:ip27-prom-6.156", self.registry),
                         ["missing-locator", "firmware", "obtain-only"])

    def test_tree_citations(self):
        self.assertEqual(classes(
            "linux:db2ddb871435 arch/mips/include/asm/sn/ioc3.h", self.registry), [])
        self.assertEqual(classes("linux:db2ddb871435", self.registry), ["missing-locator"])
        self.assertEqual(classes("linux:arch/mips/include/asm/sn/ioc3.h", self.registry),
                         ["unpinned-commit"])
        self.assertEqual(classes("Mainline Linux arch/mips/pci/pci-xtalk-bridge.c: x",
                                 self.registry), ["unpinned-commit"])
        self.assertEqual(classes("linux:db2ddb8714 arch/mips/x.h", self.registry),
                         ["malformed"])
        self.assertEqual(classes("openbsd:2df91d80c104 sys/arch/sgi/x.c", self.registry),
                         ["unregistered"])
        self.assertEqual(classes("linux-ip35:9f613a321ea0 asm/sn/pic.h", self.registry),
                         ["unknown-tree"])
        # Prose that merely opens with "word:" is not a tree citation.
        self.assertEqual(classes("query-sgi-machines: the node flash items", self.registry),
                         [])

    def test_qemu_citations(self):
        with tempfile.TemporaryDirectory() as temp:
            tree = pathlib.Path(temp)
            (tree / "docs/specs").mkdir(parents=True)
            (tree / "docs/specs/page.rst").write_text(".. _a-label:\n\nTitle\n=====\n")
            self.assertEqual(classes("qemu:docs/specs/page.rst#a-label", self.registry, tree), [])
            self.assertEqual(classes("qemu:docs/specs/page.rst", self.registry, tree), [])
            self.assertEqual(classes("qemu:docs/specs/page.rst#other", self.registry, tree),
                             ["unregistered"])
            self.assertEqual(classes("qemu:docs/specs/none.rst", self.registry, tree),
                             ["unregistered"])
            self.assertEqual(classes("qemu:../outside.rst", self.registry, tree), ["malformed"])

    def test_uncitable_text_is_reported_by_class(self):
        registry = self.registry
        self.assertEqual(classes("docs/research/note.md: a finding", registry),
                         ["origami-path"])
        self.assertEqual(classes("D-0023: a decision", registry), ["decision-id"])
        self.assertEqual(classes("Origami commit 0123456789ab, tools/x.py: y", registry),
                         ["origami-path", "tool-path"])
        self.assertEqual(classes("docs/datasheets/ioc3.md the joint-buck capture", registry),
                         ["copied-datasheet", "nickname"])
        self.assertEqual(classes("the two-node capture model-toad", registry), ["nickname"])
        self.assertEqual(classes("sgi-hw/booklet.pdf Table 41", registry), ["library-path"])
        self.assertEqual(classes("inventory: https://example.org/t/1.html", registry),
                         ["bare-url"])
        self.assertEqual(citations.classify("hw/mips/sgi/sn1-node.c", registry, ROOT)[0],
                         "qemu-code")


class CheckTests(unittest.TestCase):
    def test_fact_classes(self):
        with tempfile.TemporaryDirectory() as temp:
            models = pathlib.Path(temp)
            for name in (citations.REGISTRY, citations.REGISTRY_SCHEMA):
                shutil.copy(INPUTS / name, models / name)
            (models / "layer.yaml").write_text(textwrap.dedent("""\
                sources:
                  short: "007-3708-002 printed page 5"
                facts:
                  - {provenance: vendor-document, sources: [short]}
                  - {provenance: derived, sources: ["hw/mips/sgi/octane.c"]}
                  - {provenance: derived, sources: ["The composition selects it."]}
                  - {provenance: vendor-source, sources: ["web:dmesgd-8994 line 3"]}
                  - provenance: derived
                    sources: ["web:dmesgd-8994 line 3"]
                    notes: "see docs/notebooks/x.md"
                open_questions:
                  - {id: q, question: "what does D-0024 say?"}
                """))
            report = citations.check(models, ROOT)
        self.assertEqual(report.failures, [])
        self.assertEqual((report.entries, report.facts), (5, 5))
        self.assertEqual(report.count("qemu-code-only"), 1)
        self.assertEqual(report.count("free-text-only"), 1)
        self.assertEqual(report.count("vendor-source"), 1)
        self.assertEqual(report.count("internal-note"), 2)

    def test_models_cite_only_well_formed_registered_ids(self):
        report = citations.check()
        print("\n" + report.summary(), file=sys.stderr)
        self.assertEqual(report.failures, [])


if __name__ == "__main__":
    unittest.main()
