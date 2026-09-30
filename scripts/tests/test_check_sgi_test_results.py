# SPDX-License-Identifier: GPL-2.0-or-later

import tempfile
import unittest
import importlib.util
import json
import sys
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
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

    def test_accepts_native_exitcode_test_identity(self):
        self.write_report('''<testsuites tests="1">
          <testsuite name="qemu" tests="1">
            <testcase classname="qemu"
              name="qemu:func-quick+func-mips64 / func-mips64-lib_prom"/>
          </testsuite>
        </testsuites>''')
        validate(read_report(self.junit), ['func-mips64-lib_prom'])

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
        with self.assertRaisesRegex(
                ResultError, 'skipped testcases'):
            read_report(self.junit)

    def test_allows_listed_qtest_skip_after_normalizing_tap_counter(self):
        suite = ('qemu.qemu:qtest+qtest-mips64+slow+sgi+sgi-qtest / '
                 'qtest-mips64/sn0-migration-test')
        case_name = ('99 /mips64/sn0/migration/'
                     'legacy-count2-before-first-expiry')
        self.write_report(f'''<testsuite name="{suite}" tests="1" skipped="1">
          <testcase classname="{suite}"
                    name="{case_name}">
            <skipped message="QTEST_QEMU_BINARY_IOC3_V15 is not set"/>
          </testcase>
        </testsuite>''')
        allowed = {('qtest-mips64/sn0-migration-test',
                    '/mips64/sn0/migration/legacy-count2-before-first-expiry')}
        report = read_report(self.junit, optional_skips=allowed)
        validate(report, ['qtest-mips64/sn0-migration-test'])

    def test_rejects_unlisted_qtest_skip_with_same_suite(self):
        suite = ('qemu.qemu:qtest+qtest-mips64+slow+sgi+sgi-qtest / '
                 'qtest-mips64/sn0-migration-test')
        self.write_report(f'''<testsuite name="{suite}" tests="1" skipped="1">
          <testcase classname="{suite}"
                    name="99 /mips64/sn0/migration/unlisted-case">
            <skipped message="not an approved prerequisite"/>
          </testcase>
        </testsuite>''')
        allowed = {('qtest-mips64/sn0-migration-test',
                    '/mips64/sn0/migration/legacy-count2-before-first-expiry')}
        with self.assertRaisesRegex(ResultError, 'skipped testcases'):
            read_report(self.junit, optional_skips=allowed)

    def test_allows_only_exact_mesonskipped_qtest_target_identity(self):
        identity = ('qemu', 'qemu:qtest+qtest-mips64+slow / '
                    'qtest-mips64/cdrom-test')
        self.write_report(f'''<testsuite name="qemu" tests="1" skipped="1">
          <testcase classname="qemu"
                    name="{identity[1]}">
            <skipped/>
          </testcase>
        </testsuite>''')
        report = read_report(self.junit, optional_skips={identity})
        validate(report, ['qtest-mips64/cdrom-test'])

        other_identity = ('qemu:qtest+qtest-mips64+slow / '
                          'qtest-mips64/other-test')
        self.write_report(f'''<testsuite name="qemu" tests="1" skipped="1">
          <testcase classname="qemu"
                    name="{other_identity}">
            <skipped/>
          </testcase>
        </testsuite>''')
        with self.assertRaisesRegex(ResultError, 'skipped testcases'):
            read_report(self.junit, optional_skips={identity})

    def test_reads_full_qtest_skip_manifest(self):
        manifest = (Path(__file__).parents[2] / 'tests/qtest/'
                    'mips64-qtest-optional-skips.txt')
        identity = ('qemu', 'qemu:qtest+qtest-mips64+slow / '
                    'qtest-mips64/cdrom-test')
        identities = checker.read_skip_allowlist(manifest)
        self.assertEqual(len(identities), 23)
        self.assertIn(identity, identities)
        self.assertIn(('qtest-mips64/sn0-migration-test',
                       '/mips64/sn0/migration/'
                       'legacy-count2-before-first-expiry'),
                      identities)
        self.write_report(f'''<testsuite name="qemu" tests="1" skipped="1">
          <testcase classname="qemu"
                    name="{identity[1]}">
            <skipped/>
          </testcase>
        </testsuite>''')
        self.write_expected('qtest-mips64/cdrom-test\n')
        with redirect_stdout(StringIO()):
            status = checker.main([
                '--junit', str(self.junit), '--expect', str(self.expected),
                '--allow-skips-from', str(manifest)])
        self.assertEqual(status, 0)

    def test_allows_only_optional_v9_mgras_case(self):
        suite = ('qemu.qemu:qtest+qtest-mips64+slow+sgi+sgi-qtest / '
                 'qtest-mips64/sn0-origin2000-mgras-test')
        case = ('156 /mips64/sn0/origin2000/mgras/'
                'migration-from-v9-format-default')
        for name, allowed in ((case, True),
                              ('157 /mips64/sn0/origin2000/mgras/other', False)):
            with self.subTest(name=name):
                self.write_report(f'''<testsuite name="{suite}" tests="1"
                    skipped="1">
                  <testcase classname="{suite}"
                            name="{name}">
                    <skipped message="QTEST_QEMU_BINARY_MGRAS_V9 is not set"/>
                  </testcase>
                </testsuite>''')
                if allowed:
                    validate(read_report(self.junit),
                             ['qtest-mips64/sn0-origin2000-mgras-test'])
                else:
                    with self.assertRaisesRegex(ResultError,
                                                'skipped testcases'):
                        read_report(self.junit)

    def test_rejects_failure_and_error(self):
        for tag in ('failure', 'error'):
            with self.subTest(tag=tag):
                failures = 1 if tag == 'failure' else 0
                errors = 1 if tag == 'error' else 0
                self.write_report(f'''<testsuite tests="1" failures="{failures}"
                    errors="{errors}" skipped="0">
                  <testcase name="required">
                    <{tag}>bad</{tag}>
                  </testcase>
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
        self.assertEqual(len(identities), 85)
        self.assertEqual(len(identities), len(set(identities)))

    def test_rejects_invalid_and_duplicate_rom_rows(self):
        for body, message in (
                ('bad R10000 unknown\n', 'invalid ROM option'),
                ('sext R10000\nsext R10000\n', 'duplicate ROM row'),
                ('sext R10000 R10000\n', 'invalid ROM suite row'),
                ('timed R10000 timed\n', 'invalid ROM suite row'),
                ('sext R10000 config:unknown\n', 'invalid ROM suite row')):
            with self.subTest(body=body):
                suite = self.root / 'suite'
                suite.write_text(body, encoding='utf-8')
                with self.assertRaisesRegex(ResultError, message):
                    rom_identities(suite)

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

    def test_rom_qualification_requires_registered_native_target(self):
        suite = (Path(__file__).parents[2] /
                 'tests/functional/mips64/roms/suite')
        tests = [{"name": "func-mips64-sgi_roms",
                  "suite": ["qemu:thorough"]}]
        with mock.patch.object(checker.subprocess, 'check_output',
                               return_value=json.dumps(tests)), \
                redirect_stdout(StringIO()) as output:
            status = checker.main([
                '--list-rom-identities', str(suite),
                '--require-rom-target', 'build'])
        self.assertEqual(status, 0)
        self.assertEqual(len(output.getvalue().splitlines()), 85)
        with mock.patch.object(checker.subprocess, 'check_output',
                               return_value='[]'), \
                redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            status = checker.main([
                '--list-rom-identities', str(suite),
                '--require-rom-target', 'build'])
        self.assertEqual(status, 1)


if __name__ == '__main__':
    unittest.main()
