#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check how scripts/ci/sgi-check reads tests/qtest/meson.build's tables."""

import importlib.machinery
import importlib.util
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2]
_LOADER = importlib.machinery.SourceFileLoader(
    'sgi_check', str(SOURCE / 'scripts/ci/sgi-check'))
_SPEC = importlib.util.spec_from_loader('sgi_check', _LOADER)
sgi_check = importlib.util.module_from_spec(_SPEC)
_LOADER.exec_module(sgi_check)

BASE = """\
slow_qtests = {
  # A comment about the first test.
  'a-test': 60,
  'b-test': 120,
}

qtest_priorities = {
  'a-test': 10,
}

foreach test : qtests
  test(test)
endforeach
"""


class ChangedTableTests(unittest.TestCase):
    def changed(self, new):
        return sgi_check.changed_table_tests(BASE, new)

    def test_unchanged(self):
        self.assertEqual(self.changed(BASE), set())

    def test_timeout_change_selects_that_test(self):
        self.assertEqual(
            self.changed(BASE.replace("'b-test': 120,", "'b-test': 180,")),
            {'b-test'})

    def test_priority_change_selects_that_test(self):
        self.assertEqual(
            self.changed(BASE.replace("'a-test': 10,", "'a-test': 20,")),
            {'a-test'})

    def test_added_and_removed_entries(self):
        new = BASE.replace("  'b-test': 120,\n", "  'c-test': 30,\n")
        self.assertEqual(self.changed(new), {'b-test', 'c-test'})

    def test_comment_only_change_selects_nothing(self):
        new = BASE.replace('# A comment', '# Another comment')
        self.assertEqual(self.changed(new), set())

    def test_trailing_comment_is_an_entry(self):
        new = BASE.replace("'b-test': 120,", "'b-test': 180,  # measured")
        self.assertEqual(self.changed(new), {'b-test'})

    def test_edit_outside_the_tables_is_other(self):
        self.assertIsNone(self.changed(BASE.replace('test(test)',
                                                    'test(test, x)')))

    def test_entry_without_comma_is_other(self):
        self.assertIsNone(self.changed(BASE.replace("'b-test': 120,",
                                                    "'b-test': 180")))

    def test_two_entries_on_one_line_is_other(self):
        self.assertIsNone(self.changed(BASE.replace(
            "'b-test': 120,", "'b-test': 180, 'c-test': 30,")))

    def test_multi_line_value_is_other(self):
        self.assertIsNone(self.changed(BASE.replace(
            "'b-test': 120,", "'b-test':\n    180,")))

    def test_missing_table_is_other(self):
        new = BASE.replace('qtest_priorities = {\n  \'a-test\': 10,\n}\n', '')
        self.assertIsNone(self.changed(new))

    def test_tree_meson_build_parses(self):
        text = (SOURCE / sgi_check.QTEST_MESON).read_text(encoding='utf-8')
        tables, _ = sgi_check.qtest_tables(text)
        self.assertIsNotNone(tables)
        self.assertEqual(set(tables), set(sgi_check.QTEST_TABLES))
        self.assertTrue(all(tables.values()))


if __name__ == '__main__':
    unittest.main()
