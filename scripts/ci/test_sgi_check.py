#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check scripts/ci/sgi-check's qtest selection, sharding and results."""

import importlib.machinery
import argparse
import contextlib
import importlib.util
import io
import json
import os
import unittest
from unittest import mock
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


class QtestShards(unittest.TestCase):
    DURATIONS = {'a': 600, 'b': 300, 'c': 300, 'd': 100, 'e': 50}

    def test_shards_cover_every_test_once(self):
        tests = list(self.DURATIONS) + ['new-1', 'new-2']
        shards, _ = sgi_check.qtest_shards(tests, self.DURATIONS, 3)
        flat = [test for shard in shards for test in shard]
        self.assertEqual(sorted(flat), sorted(tests))

    def test_split_ignores_input_order(self):
        tests = list(self.DURATIONS)
        forward, _ = sgi_check.qtest_shards(tests, self.DURATIONS, 2)
        backward, _ = sgi_check.qtest_shards(tests[::-1], self.DURATIONS, 2)
        self.assertEqual(forward, backward)

    def test_longest_first_balances(self):
        shards, loads = sgi_check.qtest_shards(list(self.DURATIONS),
                                               self.DURATIONS, 2)
        self.assertEqual(shards, [['a', 'd'], ['b', 'c', 'e']])
        self.assertEqual(loads, [700, 650])

    def test_unmeasured_test_counts_as_the_median(self):
        _, loads = sgi_check.qtest_shards(['a', 'new'], {'a': 10, 'b': 30},
                                          1)
        self.assertEqual(loads, [20])

    def test_tree_durations_split_evenly(self):
        durations = sgi_check.read_durations(sgi_check.QTEST_DURATIONS)
        shards, loads = sgi_check.qtest_shards(list(durations), durations, 2)
        self.assertTrue(all(shards))
        self.assertLess(max(loads) / min(loads), 1.05)

    def test_tree_durations_carry_the_generated_header(self):
        text = sgi_check.QTEST_DURATIONS.read_text(encoding='utf-8')
        self.assertTrue(text.startswith(sgi_check.DURATIONS_HEADER))


class FullPart(unittest.TestCase):
    def test_accepted(self):
        for part in ('functional', 'qtest-1-of-2', 'qtest-2-of-2',
                     'qtest-3-of-10'):
            self.assertEqual(sgi_check.full_part(part), part)

    def test_rejected(self):
        for part in ('qtest-3-of-2', 'qtest-0-of-2', 'qtest-1', 'qtest',
                     'functional-1', 'qtest-1-of-0'):
            with self.assertRaises(argparse.ArgumentTypeError):
                sgi_check.full_part(part)


class NeedsPassed(unittest.TestCase):
    JOBS = ['full-part']

    def verdict(self, needs, jobs=None):
        env = {'NEEDS': needs if isinstance(needs, str) else
               json.dumps(needs)}
        args = argparse.Namespace(jobs=jobs or self.JOBS)
        with mock.patch.dict(os.environ, env), \
                contextlib.redirect_stdout(io.StringIO()):
            try:
                sgi_check.needs_passed(args)
            except SystemExit as exc:
                return str(exc)
        return None

    def needs(self, result):
        return {'full-part': {'result': result, 'outputs': {}}}

    def test_success_passes(self):
        self.assertIsNone(self.verdict(self.needs('success')))

    def test_every_other_result_fails(self):
        for result in ('failure', 'skipped', 'cancelled', '', None):
            with self.subTest(result=result):
                self.assertIsNotNone(self.verdict(self.needs(result)))

    def test_one_failure_among_several_fails(self):
        needs = {'a': {'result': 'success'}, 'b': {'result': 'failure'}}
        self.assertIn('b (failure)', self.verdict(needs, ['a', 'b']))

    def test_missing_job_fails(self):
        self.assertIn('full-part (missing)', self.verdict({}))
        self.assertIn('full-part (missing)', self.verdict(''))

    def test_malformed_needs_fails(self):
        self.assertIsNotNone(self.verdict('not json'))
        self.assertIsNotNone(self.verdict('[]'))
        self.assertIsNotNone(self.verdict({'full-part': 'success'}))


class FailingStepReported(unittest.TestCase):
    RUN = 'https://forge/o/r/actions/runs/7'

    def status(self, statuses):
        args = argparse.Namespace(run_url=self.RUN)
        text = statuses if isinstance(statuses, str) else json.dumps(statuses)
        with mock.patch('sys.stdin', io.StringIO(text)), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            try:
                sgi_check.failing_step_reported(args)
            except SystemExit as exc:
                return exc.code
        return None

    def test_this_runs_report_counts(self):
        self.assertEqual(self.status([
            {'context': 'merge-queue/last-run', 'target_url': ''},
            {'context': 'ci/failing-step', 'target_url': self.RUN,
             'description': 'qtest-1-of-2: run this part'}]), 0)

    def test_another_runs_report_does_not(self):
        self.assertEqual(self.status([
            {'context': 'ci/failing-step',
             'target_url': 'https://forge/o/r/actions/runs/6'}]), 1)

    def test_no_statuses(self):
        self.assertEqual(self.status([]), 1)

    def test_unreadable_statuses(self):
        self.assertEqual(self.status('<html>'), 2)
        self.assertEqual(self.status({'message': 'not found'}), 2)


if __name__ == '__main__':
    unittest.main()
