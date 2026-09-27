# SPDX-License-Identifier: GPL-2.0-or-later

import tempfile
import unittest
import importlib.util
import json
import sys
from unittest import mock
from pathlib import Path

CHECKER_PATH = Path(__file__).parents[1] / 'check-sgi-test-results.py'
spec = importlib.util.spec_from_file_location(
    'check_sgi_test_results', CHECKER_PATH)
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)

ResultError = checker.ResultError
read_expected = checker.read_expected
read_report = checker.read_report
rom_identities = checker.rom_identities
meson_test_identities = checker.meson_test_identities
validate = checker.validate


class CheckSgiTestResults(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.junit = self.root / 'testlog.junit.xml'
        self.expected = self.root / 'expected.txt'

    def tearDown(self):
        self.tempdir.cleanup()

    def write_report(self, body):
        self.junit.write_text(body, encoding='utf-8')

    def write_expected(self, body):
        self.expected.write_text(body, encoding='utf-8')

    def test_accepts_required_tap_subcases(self):
        self.write_report('''<testsuites tests="2" failures="0"
            errors="0" skipped="0">
          <testsuite name="functional" tests="2" failures="0"
              errors="0" skipped="0">
            <testcase name="OriginPromMatrix.test_prom_full_boot_1cpu"/>
            <testcase classname="IrixMiniroot" name="test_irix_miniroot"/>
          </testsuite>
        </testsuites>''')
        self.write_expected('OriginPromMatrix.test_prom_full_boot_1cpu\n'
                            'IrixMiniroot::test_irix_miniroot\n')

        expected = read_expected(self.expected)
        report = read_report(self.junit)
        validate(report, expected)

    def test_accepts_parent_suite_with_emitted_cases(self):
        suite = ('qemu.qemu:func-thorough+func-mips64-thorough+thorough / '
                 'func-mips64-sgi_roms')
        self.write_report(f'''<testsuites tests="2" failures="0"
            errors="0" skipped="0">
          <testsuite name="{suite}"
                     tests="2" failures="0" errors="0" skipped="0">
            <testcase classname="{suite}"
                      name="1 test_sgi_roms.SGIRoms.test_sext_R10000_default"/>
            <testcase classname="{suite}"
              name="2 test_sgi_roms.SGIRoms.test_llscscale_R10000_default"/>
          </testsuite>
        </testsuites>''')
        report = read_report(self.junit)
        validate(report, ['func-mips64-sgi_roms',
                           'SGIRoms.test_sext_R10000_default',
                           'SGIRoms.test_llscscale_R10000_default'])

    def test_rejects_empty_or_missing_report(self):
        self.write_report('<testsuites tests="0"/>')
        self.write_expected('case\n')
        with self.assertRaisesRegex(ResultError, 'no testcases'):
            read_report(self.junit)
        self.junit.unlink()
        with self.assertRaisesRegex(ResultError, 'does not exist'):
            read_report(self.junit)

    def test_rejects_skip_even_when_another_case_passes(self):
        self.write_report('''<testsuite tests="2" failures="0"
            errors="0" skipped="1">
          <testcase name="required"/>
          <testcase name="optional">
            <skipped message="asset unavailable"/>
          </testcase>
        </testsuite>''')
        with self.assertRaisesRegex(ResultError, 'skipped testcases'):
            read_report(self.junit)

    def test_rejects_failure_and_error(self):
        for tag in ('failure', 'error'):
            with self.subTest(tag=tag):
                failures = 1 if tag == 'failure' else 0
                errors = 1 if tag == 'error' else 0
                self.write_report(f'''<testsuite tests="1" failures="{failures}"
                    errors="{errors}" skipped="0">
                  <testcase name="required"><{tag}>bad</{tag}></testcase>
                </testsuite>''')
                with self.assertRaisesRegex(ResultError, 'failed testcases'):
                    read_report(self.junit)

    def test_rejects_incomplete_or_ambiguous_report(self):
        self.write_report('''<testsuite tests="1" failures="0"
            errors="0" skipped="0">
          <testcase name="actual"/>
        </testsuite>''')
        report = read_report(self.junit)
        with self.assertRaisesRegex(ResultError, 'missing'):
            validate(report, ['required'])
        with self.assertRaisesRegex(ResultError, 'declares 2 tests'):
            self.write_report('''<testsuite tests="2">
              <testcase name="actual"/>
            </testsuite>''')
            read_report(self.junit)

    def test_rejects_duplicate_expected_identities(self):
        self.write_expected('case\ncase\n')
        with self.assertRaisesRegex(ResultError, 'duplicate expected'):
            read_expected(self.expected)

    def test_lists_every_rom_case_from_the_native_manifest(self):
        manifest = (Path(__file__).parents[2] /
                    'tests/functional/mips64/roms/suite')
        identities = rom_identities(manifest)
        self.assertIn('SGIRoms.test_llscscale_R10000_default', identities)
        self.assertIn('SGIRoms.test_llscscale_R10000_4cpu_2node', identities)
        self.assertEqual(len(identities), len(set(identities)))

    def test_lists_registered_tests_by_native_suite(self):
        tests = [
            {"name": "func-mips64-quick",
             "suite": ["qemu:func-quick", "qemu:func-mips64"]},
            {"name": "func-mips64-thorough",
             "suite": ["qemu:func-thorough", "qemu:func-mips64-thorough"]},
            {"name": "func-mips64-diagnostic",
             "suite": ["qemu:func-thorough", "qemu:func-mips64-thorough",
                       "qemu:func-diag"]},
            {"name": "func-mips64-install",
             "suite": ["qemu:func-thorough", "qemu:func-mips64-thorough",
                       "qemu:func-install"]},
            {"name": "qtest-aarch64/device-test", "suite": ["qemu:qtest"]},
        ]
        with mock.patch.object(checker.subprocess, 'check_output',
                               return_value=json.dumps(tests)):
            selected = meson_test_identities(
                Path('build'), suites=['func-mips64', 'func-mips64-thorough'],
                excluded_suites=['func-diag', 'func-install'])
        self.assertEqual(
            selected, ['func-mips64-quick', 'func-mips64-thorough'])

    def test_lists_registered_tests_by_exact_names(self):
        tests = [{"name": "func-mips64-one", "suite": ["qemu:func-diag"]},
                 {"name": "func-mips64-two", "suite": ["qemu:func-diag"]}]
        with mock.patch.object(checker.subprocess, 'check_output',
                               return_value=json.dumps(tests)):
            self.assertEqual(
                meson_test_identities(Path('build'), names=['func-mips64-two']),
                ['func-mips64-two'])
        with mock.patch.object(checker.subprocess, 'check_output',
                               return_value=json.dumps(tests)):
            with self.assertRaisesRegex(ResultError, 'not registered'):
                meson_test_identities(
                    Path('build'), names=['func-mips64-missing'])


if __name__ == '__main__':
    unittest.main()
