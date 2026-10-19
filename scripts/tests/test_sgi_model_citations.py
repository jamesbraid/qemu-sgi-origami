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
        self.assertEqual(classes(
            "linux-ip35:9f613a321ea03fb91bd9947cca7dda2888ccb447 "
            "arch/mips/include/asm/sn/pic.h", self.registry), [])
        # A tree with no known public location is reported, not trusted.
        homeless = copy.deepcopy(self.registry)
        homeless["trees"]["linux-ip35"] = {
            "title": "t", "url": None, "public_location": "unknown", "note": "n"}
        self.assertEqual(classes("linux-ip35:9f613a321ea0 asm/sn/pic.h", homeless),
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
        self.assertEqual(classes("docs/fictional-source-fixture.md: a finding", registry),
                         ["origami-path"])
        self.assertEqual(classes("D-0023: a decision", registry), ["decision-id"])
        self.assertEqual(classes("Origami commit 0123456789ab, tools/x.py: y", registry),
                         ["origami-path", "tool-path"])
        self.assertEqual(classes("docs/fictional-contract-fixture.md the joint-buck capture", registry),
                         ["origami-path", "nickname"])
        self.assertEqual(classes("the two-node capture model-toad", registry), ["nickname"])
        self.assertEqual(classes("sgi-hw/booklet.pdf Table 41", registry), ["library-path"])
        self.assertEqual(classes("inventory: https://example.org/t/1.html", registry),
                         ["bare-url"])
        self.assertEqual(citations.classify("hw/mips/sgi/sn1-node.c", registry, ROOT)[0],
                         "qemu-code")


class CheckTests(unittest.TestCase):
    def check_layer(self, layer, registry=None):
        with tempfile.TemporaryDirectory() as temp:
            models = pathlib.Path(temp)
            for name in (citations.REGISTRY, citations.REGISTRY_SCHEMA):
                shutil.copy(INPUTS / name, models / name)
            if registry is not None:
                (models / citations.REGISTRY).write_text(yaml.safe_dump(registry))
            (models / "layer.yaml").write_text(textwrap.dedent(layer))
            return citations.check(models, ROOT)

    def test_each_failing_class_fails_the_check(self):
        def fact(source, provenance="derived"):
            return f"facts:\n  - {{provenance: {provenance}, sources: [{source!r}]}}\n"
        cases = {
            "malformed": fact("web:Bad_Key page 1"),
            "unregistered": fact("web:no-such-key page 1"),
            "origami-path": fact("docs/fictional-source-fixture.md: a finding"),
            "decision-id": fact("D-0023: a decision"),
            "tool-path": fact("see tools/x.py"),
            "library-path": fact("sgi-hw/booklet.pdf Table 41"),
            "bare-url": fact("see https://example.org/x"),
            "nickname": fact("the two-node capture model-toad"),
            "vendor-source": fact("web:dmesgd-8994 line 3", "vendor-source"),
            "qemu-code-only": fact("hw/mips/sgi/octane.c"),
            "free-text-only": fact("The composition selects it."),
            "missing-locator": fact("007-3708-002"),
            "unpinned-commit": fact("linux:arch/mips/include/asm/sn/ioc3.h"),
            "internal-note": "notes: see docs/fictional-note-fixture.md\n",
        }
        self.assertEqual(set(cases) | {"unknown-tree"}, set(citations.FAILING))
        for cls, layer in cases.items():
            with self.subTest(cls=cls):
                report = self.check_layer(layer)
                self.assertGreaterEqual(report.count(cls), 1)
                self.assertTrue(any(line.startswith(f"{cls}:")
                                    for line in report.failures), report.failures)

    def test_a_tree_with_no_public_location_fails_the_check(self):
        registry = citations.load_registry(INPUTS)
        registry["trees"]["linux-ip35"] = {
            "title": "t", "url": None, "public_location": "unknown", "note": "n"}
        report = self.check_layer(
            "facts:\n  - {provenance: derived, sources: "
            "['linux-ip35:9f613a321ea0 asm/sn/pic.h']}\n", registry)
        self.assertEqual(report.count("unknown-tree"), 1)
        self.assertTrue(report.failures)

    def test_comments_fail_the_check(self):
        report = self.check_layer(
            "# run it with tools/origami\nfacts: []  # see PR 12\n")
        self.assertEqual(report.count("internal-note"), 2)
        self.assertEqual(len(report.failures), 2)

    def test_markdown_document_in_prose_fails_the_check(self):
        # QEMU's own documentation is reStructuredText, so a Markdown path
        # under docs/ names a document that does not ship with this tree.
        report = self.check_layer(
            "facts:\n  - {provenance: derived, sources: ['web:dmesgd-8994 line 3'],"
            " description: 'see docs/fictional-guide-fixture.md for the listing'}\n")
        self.assertEqual(report.count("internal-note"), 1)
        self.assertTrue(report.failures)

    def test_qemu_rst_document_in_prose_passes(self):
        report = self.check_layer(
            "facts:\n  - {provenance: derived, sources: ['web:dmesgd-8994 line 3'],"
            " description: 'see docs/specs/sgi-sn.rst'}\n")
        self.assertEqual(report.failures, [])

    def test_informational_classes_do_not_fail(self):
        report = self.check_layer(
            "facts:\n"
            "  - {provenance: observed-hardware, sources: "
            "['firmware:ip27-prom-6.156 string \"x\"']}\n"
            "  - {provenance: vendor-document, sources: "
            "['web:nekonomicon-16727716 post 2']}\n")
        self.assertEqual(report.count("firmware"), 1)
        self.assertEqual(report.count("obtain-only"), 2)
        self.assertEqual(report.failures, [])
        self.assertEqual(set(citations.INFORMATIONAL), {"firmware", "obtain-only"})

    def test_clean_citations_pass(self):
        report = self.check_layer(
            "sources:\n  short: '007-3708-002 printed page 5'\n"
            "facts:\n  - {provenance: vendor-document, sources: [short]}  # fine\n"
            "  - {provenance: derived, sources: ['web:dmesgd-8994 line 3']}\n")
        self.assertEqual(report.failures, [])
        self.assertEqual((report.entries, report.facts), (2, 2))

    def test_comments_are_found_outside_quotes(self):
        text = 'a: "x # not a comment"\nb: 1  # a comment\n# whole line\nc: it#s\n'
        self.assertEqual(list(citations.yaml_comments(text)),
                         [(2, "# a comment"), (3, "# whole line")])

    def test_models_cite_only_well_formed_registered_ids(self):
        report = citations.check()
        print("\n" + report.summary(), file=sys.stderr)
        self.assertEqual(report.failures, [])


if __name__ == "__main__":
    unittest.main()
