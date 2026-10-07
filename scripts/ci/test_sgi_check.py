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
import sys
import tempfile
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
        new = BASE.replace("slow_qtests = {", "other_qtests = {")
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

    def test_preload_lightens_that_shard(self):
        shards, loads = sgi_check.qtest_shards(
            list(self.DURATIONS), self.DURATIONS, 2, preload=[500, 0])
        self.assertEqual(shards, [['b', 'd', 'e'], ['a', 'c']])
        self.assertEqual(loads, [950, 900])

    def test_tree_durations_split_evenly(self):
        durations = sgi_check.read_durations(sgi_check.QTEST_DURATIONS)
        for count in (2, 3):
            shards, loads = sgi_check.qtest_shards(
                list(durations), durations, count,
                sgi_check.functional_preload(count))
            self.assertTrue(all(shards))
            self.assertLess(max(loads) / min(loads), 1.05)

    def test_functional_share_is_share_one_of_a_real_split(self):
        self.assertEqual(sgi_check.functional_preload(1), [0])
        self.assertEqual(sgi_check.functional_preload(3),
                         [sgi_check.FUNCTIONAL_LOAD_S, 0, 0])


class QtestPartition(unittest.TestCase):
    """Every qtest runs in exactly one part, whatever the part count."""

    def parts(self, tests, durations, count):
        """Each job's qtests, computed as each job does, from its own run."""
        return [sgi_check.qtest_shards(
                    list(reversed(tests)) if index % 2 else tests,
                    durations, count,
                    sgi_check.functional_preload(count))[0][index]
                for index in range(count)]

    def check(self, tests, durations, count):
        parts = self.parts(tests, durations, count)
        flat = [test for part in parts for test in part]
        self.assertEqual(sorted(flat), sorted(tests))
        self.assertEqual(len(flat), len(set(flat)))

    def test_every_count_runs_each_test_once(self):
        durations = sgi_check.read_durations(sgi_check.QTEST_DURATIONS)
        tests = sorted(durations) + ['qtest-mips64/new-1',
                                     'qtest-mips64/new-2']
        for count in range(1, 9):
            with self.subTest(count=count):
                self.check(tests, durations, count)

    def test_few_tests_leave_a_part_empty_not_duplicated(self):
        self.check(['t1', 't2'], {'t1': 5, 't2': 5}, 3)

    def test_functional_job_takes_the_least_measured_qtest_time(self):
        durations = sgi_check.read_durations(sgi_check.QTEST_DURATIONS)
        shards, _ = sgi_check.qtest_shards(
            list(durations), durations, 3, sgi_check.functional_preload(3))
        times = [sum(durations[t] for t in shard) for shard in shards]
        self.assertEqual(times.index(min(times)), 0)

    def test_tree_durations_carry_the_generated_header(self):
        text = sgi_check.QTEST_DURATIONS.read_text(encoding='utf-8')
        self.assertTrue(text.startswith(sgi_check.DURATIONS_HEADER))
        self.assertIn('# Source: ', text)


class QtestDurations(unittest.TestCase):
    CONSOLE = (
        '2026-10-06T23:53:11.6Z  1/69 qemu:qtest+qtest-mips64+slow / '
        'qtest-mips64/qom-test      OK     36.90s   15 subtests passed\n'
        '2026-10-06T23:53:14.4Z  2/69 qemu:qtest+qtest-mips64+sgi / '
        'qtest-mips64/sn0-hub-test  SKIP    0.01s\n'
        '2026-10-06T23:53:14.5Z  1/44 qemu:unit+sgi / test-onewire   OK   '
        '0.06s\n')

    def test_console_lines_name_only_qtests(self):
        self.assertEqual(list(sgi_check.log_durations(self.CONSOLE)),
                         [('qtest-mips64/qom-test', 36.9),
                          ('qtest-mips64/sn0-hub-test', 0.01)])

    def test_json_lines(self):
        text = json.dumps({'name': 'qtest-mips64/a', 'duration': 2.5}) + '\n' \
            + json.dumps({'name': 'func-x', 'duration': 9}) + '\n'
        self.assertEqual(list(sgi_check.log_durations(text)),
                         [('qtest-mips64/a', 2.5)])

    def test_median_across_logs(self):
        logs = [f'1/2 s / qtest-mips64/a   OK   {seconds}s\n'
                for seconds in (10, 100, 12)]
        logs.append('1/2 s / qtest-mips64/b   OK   7s\n')
        self.assertEqual(sgi_check.median_durations(logs),
                         {'qtest-mips64/a': 12, 'qtest-mips64/b': 7})


class RunConcurrently(unittest.TestCase):
    def test_commands_overlap_up_to_the_worker_count(self):
        # Each command waits until the other has started, so this passes
        # only if two run at once, and times out if they ran one by one.
        script = ('import pathlib, sys, time\n'
                  'me, other = map(pathlib.Path, sys.argv[1:3])\n'
                  'me.touch()\n'
                  'end = time.time() + 20\n'
                  'while not other.exists():\n'
                  '    assert time.time() < end, "never overlapped"\n'
                  '    time.sleep(0.01)\n')
        with tempfile.TemporaryDirectory() as tmp, \
                contextlib.redirect_stdout(io.StringIO()):
            a, b = Path(tmp, 'a'), Path(tmp, 'b')
            codes = sgi_check.run_concurrently(
                [('one', [sys.executable, '-c', script, a, b]),
                 ('two', [sys.executable, '-c', script, b, a])], 2, tmp)
        self.assertEqual(codes, {'one': 0, 'two': 0})

    def test_one_worker_runs_one_at_a_time(self):
        script = ('import pathlib, sys, time\n'
                  'lock = pathlib.Path(sys.argv[1])\n'
                  'assert not lock.exists(), "overlapped"\n'
                  'lock.touch(); time.sleep(0.2); lock.unlink()\n')
        with tempfile.TemporaryDirectory() as tmp, \
                contextlib.redirect_stdout(io.StringIO()):
            lock = Path(tmp, 'lock')
            codes = sgi_check.run_concurrently(
                [(name, [sys.executable, '-c', script, lock])
                 for name in 'abc'], 1, tmp)
        self.assertEqual(codes, {'a': 0, 'b': 0, 'c': 0})

    def test_every_exit_status_and_output_is_kept(self):
        out = io.StringIO()
        with tempfile.TemporaryDirectory() as tmp, \
                contextlib.redirect_stdout(out):
            codes = sgi_check.run_concurrently(
                [('ok', [sys.executable, '-c', 'print("fine")']),
                 ('bad', [sys.executable, '-c',
                          'import sys; print("broke"); sys.exit(3)'])],
                2, tmp)
        self.assertEqual(codes, {'ok': 0, 'bad': 3})
        self.assertIn('fine\n', out.getvalue())
        self.assertIn('broke\n', out.getvalue())
        self.assertIn('finish bad: exit 3', out.getvalue())


class FakeBuild(sgi_check.Build):
    """A Build whose Meson runs are Python one-liners."""

    def __init__(self, path, exits):
        super().__init__(path)
        self.exits = exits
        self.checked = []
        self.built = []

    def meson_command(self, logbase, args, *a, **kw):
        return [sys.executable, '-c',
                f'import sys; sys.exit({self.exits[logbase]})']

    def check_report(self, logbase, expect):
        self.checked.append((logbase, expect))
        if logbase in self.exits and self.exits[logbase] == 'report':
            sys.exit('bad report')

    def test_prerequisites(self, names=(), suites=()):
        return ['tool']

    def ninja(self, *targets):
        self.built.append(targets)


class MesonTestsConcurrently(unittest.TestCase):
    JOBS = [('a', ['x'], 'a.expected'), ('b', ['y'], 'b.expected'),
            ('c', ['z'], 'c.expected')]

    def run_jobs(self, exits):
        with tempfile.TemporaryDirectory() as tmp, \
                contextlib.redirect_stdout(io.StringIO()):
            build = FakeBuild(Path(tmp), exits)
            try:
                build.meson_tests_concurrently(self.JOBS, 3)
                error = None
            except SystemExit as exc:
                error = str(exc)
        return build, error

    def test_all_pass(self):
        build, error = self.run_jobs({'a': 0, 'b': 0, 'c': 0})
        self.assertIsNone(error)
        self.assertEqual(build.built, [('tool',)])
        self.assertEqual(sorted(build.checked),
                         [('a', 'a.expected'), ('b', 'b.expected'),
                          ('c', 'c.expected')])

    def test_every_failure_is_reported_and_every_report_checked(self):
        build, error = self.run_jobs({'a': 1, 'b': 0, 'c': 2})
        self.assertEqual(error, 'failed: a, c')
        self.assertEqual(len(build.checked), 3)

    def test_a_bad_report_fails_a_passing_run(self):
        build, error = self.run_jobs({'a': 0, 'b': 'report', 'c': 0})
        self.assertEqual(error, 'failed: b')
        self.assertEqual(len(build.checked), 3)


class FullFunctional(unittest.TestCase):
    """The parallel functional part keeps every run and its expectations."""

    def jobs(self):
        captured = {}

        def concurrently(build, jobs, workers, names=(), suites=()):
            captured.update(jobs=jobs, workers=workers, names=names,
                            suites=suites)

        def required(build, module, test_class, dest, selected=None):
            dest.write_text(f'{test_class}\n', encoding='utf-8')
            return ['--test-args', 'sel'] if selected else []

        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(sgi_check.Build, 'ninja'), \
                mock.patch.object(sgi_check, 'run'), \
                mock.patch.object(sgi_check, 'required_cases', required), \
                mock.patch.object(sgi_check.Build, 'list_meson_tests'), \
                mock.patch.object(sgi_check.Build,
                                  'meson_tests_concurrently', concurrently):
            sgi_check.full_functional(sgi_check.Build(Path(tmp)))
        return captured

    def test_every_invocation_is_kept_with_its_expectation(self):
        got = self.jobs()
        by_logbase = {logbase: (args, Path(expected).name)
                      for logbase, args, expected in got['jobs']}
        want = {logbase: ([*(['--test-args', 'sel'] if selected else []),
                           target], f'{logbase}.expected')
                for target, logbase, _, _, selected
                in sgi_check.FULL_FUNCTIONAL}
        want['full-rom'] = (['func-mips64-sgi_roms'], 'full-rom.expected')
        want['full-unit'] = (['--suite', 'qemu:sgi-unit'],
                             'full-unit.expected')
        self.assertEqual(by_logbase, want)
        self.assertEqual(len(got['jobs']), len(want))

    def test_the_longest_start_first_and_workers_are_bounded(self):
        got = self.jobs()
        self.assertEqual([logbase for logbase, *_ in got['jobs']][:4],
                         list(sgi_check.LONGEST_FUNCTIONAL_FIRST))
        self.assertEqual(got['workers'], sgi_check.FUNCTIONAL_PROCESSES)
        self.assertIn('func-mips64-sgi_roms', got['names'])
        self.assertEqual(got['suites'], ['sgi-unit'])


class TestPrerequisites(unittest.TestCase):
    def test_selected_tests_name_their_dependencies_only(self):
        tests = [{'name': 'func-a', 'suite': ['qemu:func'], 'depends': ['t1']},
                 {'name': 'func-b', 'suite': ['qemu:func'], 'depends': ['t2']},
                 {'name': 'unit-1', 'suite': ['qemu:sgi-unit'],
                  'depends': ['t3', 't1']}]
        targets = [{'id': 't1', 'filename': ['/b/one']},
                   {'id': 't2', 'filename': ['/b/two']},
                   {'id': 't3', 'filename': ['/b/sub/three']}]
        answers = iter([json.dumps(tests), json.dumps(targets)])
        build = sgi_check.Build(Path('/b'))
        with mock.patch.object(sgi_check, 'output',
                               lambda *a, **kw: next(answers)):
            got = build.test_prerequisites(['func-a'], ['sgi-unit'])
        self.assertEqual(got, ['one', 'sub/three'])


class SmokeQtestSetup(unittest.TestCase):
    """Pull requests run the quick setup; the full run stays thorough."""

    # a-test is quick, b-test is in the slow suite.
    SUITES = {'qtest-mips64/a-test': {'qtest', 'qtest-mips64'},
              'qtest-mips64/b-test': {'qtest', 'qtest-mips64', 'slow'}}

    def run_smoke(self, qtests):
        calls = []
        self.out = io.StringIO()

        def list_meson_tests(build, dest, *selection):
            calls.append(('list', None, selection))
            Path(dest).write_text('expected\n', encoding='utf-8')

        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(sgi_check, 'script_suites'), \
                mock.patch.object(sgi_check, 'rom_suite') as rom, \
                mock.patch.object(sgi_check, 'required_cases',
                                  return_value=[]), \
                mock.patch.object(sgi_check, 'select_qtests',
                                  return_value=qtests), \
                mock.patch.object(sgi_check.Build, 'ninja') as ninja, \
                mock.patch.object(sgi_check.Build, 'registered_tests',
                                  return_value=self.SUITES), \
                mock.patch.object(sgi_check.Build, 'list_meson_tests',
                                  list_meson_tests), \
                mock.patch.object(sgi_check.Build, 'meson_test',
                                  lambda self, logbase, args, *a, **kw:
                                  calls.append((logbase, args, kw))), \
                contextlib.redirect_stdout(self.out):
            sgi_check.smoke(argparse.Namespace(build=Path(tmp), base=None))
            selected = (Path(tmp) / 'selected-qtests.txt').read_text()
        # The boots and the ROM suite belong to the full run; only the
        # quick generated-ROM tests run here.
        rom.assert_not_called()
        built = ninja.call_args.args
        self.assertFalse([t for t in built if 'linux' in t or 'smp.' in t])
        return calls, selected

    def test_qtests_run_quick(self):
        calls, _ = self.run_smoke(['qtest-mips64/a-test'])
        runs = {logbase: kw for logbase, _, kw in calls if logbase != 'list'}
        rom = sorted(f'smoke-{logbase}' for _, logbase, *_
                     in sgi_check.GENERATED_ROM_FUNCTIONAL)
        self.assertEqual(sorted(runs), sorted(['smoke-qtest', 'smoke-unit']
                                              + rom))
        self.assertEqual(runs['smoke-qtest']['setup'], 'qemu:quick')
        self.assertNotIn('setup', runs['smoke-unit'])

    def test_only_the_tests_quick_keeps_are_named(self):
        calls, selected = self.run_smoke(['qtest-mips64/a-test',
                                          'qtest-mips64/b-test'])
        self.assertEqual(selected,
                         'qtest-mips64/a-test\nqtest-mips64/b-test\n')
        lists = [sel for kind, _, sel in calls if kind == 'list']
        self.assertEqual(lists[0][0], '--names-from')
        args = {logbase: args for logbase, args, _ in calls}
        self.assertEqual(args['smoke-qtest'], ['qtest-mips64/a-test'])

    def test_over_budget_selection_runs_only_what_fits(self):
        suites = {f'qtest-mips64/{n}': {'qtest-mips64'} for n in 'abc'}
        durations = {'qtest-mips64/a': 100, 'qtest-mips64/b': 200,
                     'qtest-mips64/c': 300}
        with mock.patch.object(sgi_check, 'SMOKE_QTEST_BUDGET_S', 300), \
                mock.patch.object(sgi_check, 'read_durations',
                                  return_value=durations), \
                mock.patch.object(self, 'SUITES', suites):
            calls, _ = self.run_smoke(sorted(suites))
        args = {logbase: args for logbase, args, _ in calls}
        self.assertEqual(args['smoke-qtest'],
                         ['qtest-mips64/a', 'qtest-mips64/b'])
        self.assertIn('over the 300 s budget): qtest-mips64/c',
                      self.out.getvalue())

    def test_no_selected_qtest_runs_none(self):
        for qtests in ([], ['qtest-mips64/b-test']):
            with self.subTest(qtests=qtests):
                calls, _ = self.run_smoke(qtests)
                self.assertNotIn('smoke-qtest',
                                 [logbase for logbase, _, _ in calls])
                self.assertIn('smoke-unit',
                              [logbase for logbase, _, _ in calls])


class FitBudget(unittest.TestCase):
    DURATIONS = {'a': 100, 'b': 200, 'c': 50, 'd': 300}

    def test_under_budget_keeps_all(self):
        kept, left = sgi_check.fit_budget(['b', 'a'], self.DURATIONS, 360)
        self.assertEqual((kept, left), (['a', 'b'], []))

    def test_over_budget_keeps_the_cheapest_that_fit(self):
        kept, left = sgi_check.fit_budget(['d', 'b', 'a', 'c'],
                                          self.DURATIONS, 360)
        self.assertEqual((kept, left), (['a', 'b', 'c'], ['d']))

    def test_choice_ignores_input_order(self):
        tests = ['d', 'b', 'a', 'c']
        self.assertEqual(sgi_check.fit_budget(tests, self.DURATIONS, 300),
                         sgi_check.fit_budget(tests[::-1], self.DURATIONS,
                                              300))

    def test_unmeasured_test_counts_as_the_default(self):
        kept, left = sgi_check.fit_budget(['a', 'new'], self.DURATIONS,
                                          200, default=150)
        self.assertEqual((kept, left), (['a'], ['new']))

    def test_one_test_over_budget_runs_nothing(self):
        self.assertEqual(sgi_check.fit_budget(['d'], self.DURATIONS, 100),
                         ([], ['d']))

    def test_tree_budget_and_default_are_set(self):
        self.assertGreater(sgi_check.SMOKE_QTEST_BUDGET_S, 0)
        self.assertGreater(sgi_check.UNMEASURED_QTEST_S, 0)


class SelectQtests(unittest.TestCase):
    """A pull request runs the qtests whose own files it edits."""

    REGISTERED = {name: set() for name in (
        'qtest-mips64/a-test', 'qtest-mips64/b-test',
        'qtest-mips64/c-test')}

    def select(self, changed, base='abc', fork='fork', new_meson=None):
        """Select with `changed` as the paths the diff reports."""
        def output(cmd, **kwargs):
            if cmd[3] == 'diff':
                return '\n'.join(changed) + '\n'
            return BASE

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for path in changed:
                (root / path).parent.mkdir(parents=True, exist_ok=True)
                (root / path).write_text(
                    new_meson if path == sgi_check.QTEST_MESON and new_meson
                    else '', encoding='utf-8')
            with mock.patch.object(sgi_check, 'SOURCE', root), \
                    mock.patch.object(sgi_check, 'output', output), \
                    mock.patch.object(sgi_check, 'merge_base',
                                      return_value=fork), \
                    mock.patch.object(sgi_check.Build, 'registered_tests',
                                      return_value=self.REGISTERED):
                return sgi_check.select_qtests(
                    sgi_check.Build(Path(tmp) / 'build'), base)

    def test_model_code_selects_no_qtest(self):
        self.assertEqual(self.select(['hw/mips/sgi/ip27.c',
                                      'hw/net/xframe.c',
                                      'include/hw/mips/sgi.h']), [])

    def test_two_edited_qtests_select_exactly_those(self):
        self.assertEqual(
            self.select(['tests/qtest/a-test.c', 'tests/qtest/c-test.c',
                         'hw/mips/sgi/ip27.c']),
            ['qtest-mips64/a-test', 'qtest-mips64/c-test'])

    def test_shared_test_code_selects_nothing(self):
        self.assertEqual(self.select(['tests/qtest/libqtest.c',
                                      'tests/qtest/libqtest.h']), [])

    def test_source_the_suite_does_not_register_selects_nothing(self):
        self.assertEqual(self.select(['tests/qtest/other-arch-test.c']), [])

    def test_table_edit_selects_that_test(self):
        new = BASE.replace("'b-test': 120,", "'b-test': 180,")
        self.assertEqual(
            self.select([sgi_check.QTEST_MESON], new_meson=new),
            ['qtest-mips64/b-test'])

    def test_other_meson_edit_selects_nothing(self):
        new = BASE.replace('test(test)', 'test(test, x)')
        self.assertEqual(
            self.select([sgi_check.QTEST_MESON], new_meson=new), [])

    def test_no_base_selects_nothing(self):
        self.assertEqual(self.select(['tests/qtest/a-test.c'], base=None),
                         [])

    def test_no_merge_base_fails(self):
        with self.assertRaises(SystemExit):
            self.select(['tests/qtest/a-test.c'], fork=None)


class FullPart(unittest.TestCase):
    def test_accepted(self):
        for part in ('functional', 'qtest-1-of-2', 'qtest-2-of-2',
                     'qtest-3-of-10', 'functional+qtest-1-of-3'):
            self.assertEqual(sgi_check.full_part(part), part)

    def test_rejected(self):
        for part in ('qtest-3-of-2', 'qtest-0-of-2', 'qtest-1', 'qtest',
                     'functional-1', 'qtest-1-of-0', '',
                     'functional+', 'functional+qtest-2-of-3',
                     'functional+qtest-4-of-3', 'qtest-1-of-3+functional',
                     'functional+functional', 'qtest-1-of-2+qtest-2-of-2'):
            with self.assertRaises(argparse.ArgumentTypeError):
                sgi_check.full_part(part)


class ParsePart(unittest.TestCase):
    def test_parts(self):
        for text, want in (('functional', (True, None, None)),
                           ('qtest-2-of-3', (False, 2, 3)),
                           ('functional+qtest-1-of-3', (True, 1, 3))):
            self.assertEqual(sgi_check.parse_part(text), want)


class FullModes(unittest.TestCase):
    def run_full(self, part):
        calls = []
        with mock.patch.object(sgi_check, 'full_functional',
                               lambda build: calls.append('functional')), \
                mock.patch.object(sgi_check, 'full_qtest',
                                  lambda build, i, n: calls.append((i, n))):
            sgi_check.full(argparse.Namespace(build=Path('b'), part=part))
        return calls

    def test_each_part_runs_what_it_names(self):
        self.assertEqual(self.run_full('functional'), ['functional'])
        self.assertEqual(self.run_full('qtest-3-of-3'), [(3, 3)])
        self.assertEqual(self.run_full('functional+qtest-1-of-3'),
                         ['functional', (1, 3)])

    def test_no_part_runs_everything(self):
        self.assertEqual(self.run_full(None), ['functional', (1, 1)])


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
