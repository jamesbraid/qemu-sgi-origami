#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Performance accounting tests using only the Python standard library."""

import copy
import os
import subprocess
import sys
import unittest
from unittest import mock

import lib_performance as lib


class CpuAccountingTests(unittest.TestCase):

    def test_proc_stat_handles_spaces_and_parentheses_in_command(self):
        # Fields 14 and 15 contain 125 and 75 ticks. Child CPU is excluded.
        text = ('123 (worker ) name) S 1 2 3 4 5 6 7 8 9 10 '
                '125 75 999 888 0 0 1 0 1234\n')
        self.assertEqual(lib._parse_proc_stat(text, 100), 2.0)

    def test_proc_stat_rejects_missing_or_negative_accounting(self):
        for text in ('123 (worker) S 1',
                     '123 (worker) S 1 2 3 4 5 6 7 8 9 10 -1 0'):
            with self.subTest(text=text):
                with self.assertRaises(ValueError):
                    lib._parse_proc_stat(text, 100)

    def test_darwin_cpu_time_formats(self):
        for text, seconds in (('  00:01.25\n', 1.25),
                              ('12:34.50', 754.5),
                              ('02:03:04', 7384.0),
                              ('1-02:03:04.50', 93784.5)):
            with self.subTest(text=text):
                self.assertEqual(lib._parse_ps_time(text), seconds)

    def test_invalid_darwin_cpu_time_fails(self):
        for text in ('', 'bad', '-00:01', '01:60', '1:02:60', '1:2:3:4'):
            with self.subTest(text=text):
                with self.assertRaises(ValueError):
                    lib._parse_ps_time(text)

    def test_missing_process_is_unavailable(self):
        with mock.patch('lib_performance.sys.platform', 'linux'), \
                mock.patch('builtins.open', side_effect=FileNotFoundError):
            self.assertIsNone(lib.process_cpu_seconds(123))

    def test_unsupported_platform_is_unavailable(self):
        with mock.patch('lib_performance.sys.platform', 'win32'):
            self.assertIsNone(lib.process_cpu_seconds(123))

    @unittest.skipUnless(sys.platform in ('linux', 'darwin'),
                         'requires Linux or Darwin process accounting')
    def test_real_subprocess_accounts_for_cpu_not_waiting(self):
        program = ('import sys,time\n'
                   'print("ready", flush=True)\n'
                   'sys.stdin.readline()\n'
                   'end=time.process_time()+0.15\n'
                   'while time.process_time()<end: pass\n'
                   'print("done", flush=True)\n'
                   'sys.stdin.readline()\n')
        with subprocess.Popen([sys.executable, '-c', program],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              text=True) as child:
            try:
                self.assertEqual(child.stdout.readline().strip(), 'ready')
                before = lib.process_cpu_seconds(child.pid)
                child.stdin.write('go\n')
                child.stdin.flush()
                self.assertEqual(child.stdout.readline().strip(), 'done')
                after = lib.process_cpu_seconds(child.pid)
                self.assertIsNotNone(before)
                self.assertIsNotNone(after)
                self.assertGreaterEqual(after - before, 0.1)
            finally:
                child.stdin.write('exit\n')
                child.stdin.flush()
                child.wait(timeout=10)


class MeasurementTests(unittest.TestCase):

    def measured(self, result=True, cpu=(10.0, 12.0), wall=(20.0, 24.0)):
        with mock.patch.object(lib, 'process_cpu_seconds', side_effect=cpu), \
                mock.patch('lib_performance.time.monotonic', side_effect=wall):
            return lib.measure(123, lambda: result, 8, 'bytes')

    def test_measurement_keeps_correctness_and_normalizes_work(self):
        result = {'passed': True, 'checksum': 'abc'}
        sample = self.measured(result)
        self.assertEqual(sample, {
            'status': 'passed', 'result': result, 'units': 8, 'unit': 'bytes',
            'started_monotonic': 20.0, 'ended_monotonic': 24.0,
            'wall_seconds': 4.0, 'cpu_seconds': 2.0,
            'units_per_second': 2.0, 'cpu_seconds_per_unit': 0.25})

    def test_unsupported_cpu_accounting_is_explicit(self):
        sample = self.measured(cpu=(None, None))
        self.assertIsNone(sample['cpu_seconds'])
        self.assertIsNone(sample['cpu_seconds_per_unit'])
        self.assertEqual(sample['units_per_second'], 2.0)

    def test_failed_or_missing_correctness_never_produces_a_sample(self):
        for result in (False, None, 'PASS', 1, {}, {'passed': False},
                       {'passed': 1}):
            with self.subTest(result=result):
                with self.assertRaises(ValueError):
                    self.measured(result)

    def test_operation_exception_is_preserved(self):
        def operation():
            raise RuntimeError('checksum mismatch')
        with self.assertRaisesRegex(RuntimeError, 'checksum mismatch'):
            lib.measure(os.getpid(), operation, 8, 'bytes')

    def test_bad_work_is_refused_before_running_the_operation(self):
        def operation():
            self.fail('invalid work must not run')
        for units in (0, -1, float('nan'), float('inf'), True):
            with self.subTest(units=units):
                with self.assertRaises(ValueError):
                    lib.measure(123, operation, units, 'bytes')
        with self.assertRaises(ValueError):
            lib.measure(123, operation, 8, '')

    def test_bad_clock_or_cpu_delta_is_refused(self):
        for cpu, wall in (((2, 1), (1, 2)),
                          ((1, float('nan')), (1, 2)),
                          ((1, float('inf')), (1, 2)),
                          ((1, None), (1, 2)),
                          ((None, 1), (1, 2)),
                          ((1, 2), (1, 1)),
                          ((1, 2), (2, 1)),
                          ((1, 2), (1, float('nan')))):
            with self.subTest(cpu=cpu, wall=wall):
                with self.assertRaises(ValueError):
                    self.measured(cpu=cpu, wall=wall)


def sample(wall, cpu):
    return {'status': 'passed', 'result': True, 'units': 8, 'unit': 'bytes',
            'wall_seconds': wall, 'cpu_seconds': cpu,
            'units_per_second': 8 / wall,
            'cpu_seconds_per_unit': None if cpu is None else cpu / 8}


class SummaryTests(unittest.TestCase):

    def test_three_runs_keep_raw_data_and_sample_statistics(self):
        samples = [sample(1, 2), sample(2, 4), sample(3, 6)]
        summary = lib.summarize(samples)
        self.assertEqual(summary['samples'], samples)
        self.assertEqual(summary['count'], 3)
        self.assertEqual(summary['wall_seconds'], {
            'median': 2, 'mean': 2, 'stdev': 1, 'min': 1, 'max': 3})
        self.assertEqual(summary['cpu_seconds'], {
            'median': 4, 'mean': 4, 'stdev': 2, 'min': 2, 'max': 6})

    def test_single_run_has_zero_sample_variation(self):
        summary = lib.summarize([sample(1, 2)])
        self.assertEqual(summary['wall_seconds']['stdev'], 0.0)

    def test_unavailable_cpu_remains_null(self):
        self.assertIsNone(lib.summarize([sample(1, None), sample(2, None)])
                          ['cpu_seconds'])

    def test_empty_failed_incomplete_or_changed_work_is_refused(self):
        first = sample(1, 2)
        for changed in ({'status': 'failed'}, {'units': 16},
                        {'unit': 'blocks'},
                        {'wall_seconds': 0}, {'wall_seconds': float('inf')},
                        {'cpu_seconds': -1}, {'cpu_seconds': None},
                        {'result': False}):
            second = dict(first, **changed)
            with self.subTest(changed=changed):
                with self.assertRaises(ValueError):
                    lib.summarize([first, second])
        with self.assertRaises(ValueError):
            lib.summarize([])
        with self.assertRaises(ValueError):
            lib.summarize([{}])


class ComparisonTests(unittest.TestCase):

    def report(self, wall=2, cpu=4):
        return {'workload': 'memory',
                'config': {'machine': 'ip27', 'input_sha256': 'abc',
                           'build_flavor': 'build-fast',
                           'source_revision': 'a',
                           'binary_revision': 'b', 'binary_sha256': 'c',
                           'options': {'cpus': 1}},
                'summary': lib.summarize([sample(wall, cpu)] * 3)}

    def test_comparison_uses_medians_for_identical_configuration(self):
        base, candidate = self.report(), self.report(1, 2)
        compared = lib.compare_results(base, candidate)
        self.assertEqual(compared['workload'], 'memory')
        self.assertEqual(compared['wall_ratio'], 0.5)
        self.assertEqual(compared['cpu_ratio'], 0.5)

    def test_changed_workload_or_configuration_is_refused(self):
        base = self.report()
        for changes in ({'workload': 'disk'},
                        {'config': dict(base['config'], machine='ip30')},
                        {'config': dict(base['config'], input_sha256='other')},
                        {'config': dict(base['config'], build_flavor='build')},
                        {'config': dict(base['config'], options={'cpus': 2})},
                        {'config': dict(base['config'],
                                        binary_sha256='other')},
                        {'config': dict(base['config'],
                                        source_revision='other')},
                        {'config': dict(base['config'],
                                        binary_revision='other')}):
            with self.subTest(changes=changes):
                with self.assertRaises(ValueError):
                    lib.compare_results(base, dict(base, **changes))

    def test_failed_incomplete_or_changed_sample_is_refused(self):
        base = self.report()
        for changes in ({'status': 'failed'}, {'units': 16},
                        {'wall_seconds': 0}, {'result': False}):
            candidate = copy.deepcopy(base)
            candidate['summary']['samples'][0].update(changes)
            with self.subTest(changes=changes):
                with self.assertRaises(ValueError):
                    lib.compare_results(base, candidate)
        for summary in ({}, lib.summarize([sample(1, 2)])):
            with self.subTest(summary=summary):
                with self.assertRaises(ValueError):
                    lib.compare_results(base, dict(base, summary=summary))

    def test_unavailable_cpu_does_not_invent_a_ratio(self):
        compared = lib.compare_results(self.report(cpu=None),
                                       self.report(wall=1, cpu=None))
        self.assertIsNone(compared['cpu_ratio'])
        with self.assertRaises(ValueError):
            lib.compare_results(self.report(), self.report(cpu=None))

    def test_zero_baseline_cpu_cannot_support_a_ratio(self):
        with self.assertRaises(ValueError):
            lib.compare_results(self.report(cpu=0), self.report(cpu=0))


class AggregateComparisonTests(unittest.TestCase):

    def report(self, wall=2, cpu=4):
        return {
            'schema': 1, 'status': 'passed',
            'identity': {
                'fork': {'commit': 'base-commit', 'dirty': False},
                'qemu_binary': {'sha256': '2' * 64, 'size_bytes': 100},
                'payload': {
                    'prom': {'sha256': '1' * 64, 'size_bytes': 200},
                    'disk': {'sha256': '3' * 64, 'size_bytes': 300}}},
            'workloads': [{
                'workload': 'memory', 'status': 'passed',
                'config': {'machine': 'ip27', 'build_flavor': 'build-fast',
                           'firmware_sha256': '1' * 64,
                           'disk_sha256': '3' * 64},
                'summary': lib.summarize([sample(wall, cpu)] * 3)}]}

    def test_complete_repeated_reports_allow_different_source_and_binary(self):
        base, candidate = self.report(), self.report(1, 2)
        candidate['identity']['fork'].update(commit='candidate', dirty=True)
        candidate['identity']['qemu_binary'].update(sha256='4' * 64,
                                                    size_bytes=101)
        self.assertEqual(lib.compare_reports(base, candidate), {
            'schema': 1, 'ratios': [{'workload': 'memory',
                                    'wall_ratio': 0.5, 'cpu_ratio': 0.5}]})

    def test_failed_missing_duplicate_or_empty_workloads_are_refused(self):
        for changes in ({'schema': 2}, {'schema': True}, {'status': 'running'},
                        {'workloads': []}, {'workloads': {}},
                        {'workloads': [None]},
                        {'workloads': [{'workload': '', 'status': 'passed'}]},
                        {'workloads': [{'workload': '  ',
                                        'status': 'passed'}]},
                        {'workloads': [{'workload': 'memory',
                                        'status': 'failed'}]}):
            with self.subTest(changes=changes):
                with self.assertRaises(ValueError):
                    lib.compare_reports(self.report(),
                                        dict(self.report(), **changes))
        candidate = self.report()
        candidate['workloads'].append(copy.deepcopy(candidate['workloads'][0]))
        with self.assertRaises(ValueError):
            lib.compare_reports(self.report(), candidate)
        candidate = self.report()
        candidate['workloads'][0]['workload'] = 'other'
        with self.assertRaises(ValueError):
            lib.compare_reports(self.report(), candidate)

    def test_missing_or_malformed_identity_is_refused(self):
        identity = self.report()['identity']
        for changed in (None, [], {}, dict(identity, fork=None),
                        dict(identity, fork=[]),
                        dict(identity, fork={'commit': '', 'dirty': False}),
                        dict(identity, fork={'commit': 'x', 'dirty': None}),
                        dict(identity, fork={'commit': 'x', 'dirty': 0}),
                        dict(identity, qemu_binary=[]),
                        dict(identity, payload=None),
                        dict(identity, payload={'prom': [], 'disk': {}})):
            with self.subTest(identity=changed):
                candidate = self.report()
                candidate['identity'] = changed
                with self.assertRaises(ValueError):
                    lib.compare_reports(self.report(), candidate)
        for malformed in (None, [], {}, 'report'):
            with self.subTest(report=malformed):
                with self.assertRaises(ValueError):
                    lib.compare_reports(self.report(), malformed)

    def test_invalid_hash_or_size_is_refused_for_every_artifact(self):
        for artifact in ('qemu_binary', 'prom', 'disk'):
            for change in ({'sha256': 'short'}, {'sha256': 'g' * 64},
                           {'sha256': None}, {'size_bytes': 0},
                           {'size_bytes': -1}, {'size_bytes': True},
                           {'size_bytes': 1.5}):
                with self.subTest(artifact=artifact, change=change):
                    candidate = self.report()
                    identity = candidate['identity']
                    record = (identity['qemu_binary']
                              if artifact == 'qemu_binary'
                              else identity['payload'][artifact])
                    record.update(change)
                    with self.assertRaises(ValueError):
                        lib.compare_reports(self.report(), candidate)

    def test_actual_inputs_must_match_declared_hashes_and_each_other(self):
        for artifact, config_key in (('prom', 'firmware_sha256'),
                                     ('disk', 'disk_sha256')):
            candidate = self.report()
            candidate['identity']['payload'][artifact]['sha256'] = '4' * 64
            with self.subTest(artifact=artifact, changed='actual'):
                with self.assertRaises(ValueError):
                    lib.compare_reports(self.report(), candidate)
            candidate['workloads'][0]['config'][config_key] = '4' * 64
            with self.subTest(artifact=artifact, changed='actual and config'):
                with self.assertRaises(ValueError):
                    lib.compare_reports(self.report(), candidate)
            candidate = self.report()
            candidate['identity']['payload'][artifact]['size_bytes'] += 1
            with self.subTest(artifact=artifact, changed='size'):
                with self.assertRaises(ValueError):
                    lib.compare_reports(self.report(), candidate)

    def test_malformed_summary_or_samples_raise_value_error(self):
        for summary in (None, [], {}, {'samples': None}, {'samples': {}},
                        {'samples': [None]}):
            with self.subTest(summary=summary):
                candidate = self.report()
                candidate['workloads'][0]['summary'] = summary
                with self.assertRaises(ValueError):
                    lib.compare_reports(self.report(), candidate)


if __name__ == '__main__':
    unittest.main()
