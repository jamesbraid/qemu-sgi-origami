# SPDX-License-Identifier: BSD-3-Clause
"""Check the failing-block filter in the sanitizer workflow.

The awk program is read out of the workflow so the test cannot drift from
what runs in CI. The fixture is meson testlog text taken from a real
sanitizer run (the failing sn0-xbow-transport-test block and the timeout
block after it), with the address sanitizer report that block's abort prints
added: the original run lost that report, which is the bug under test.
"""

import re
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / '.github/workflows/sanitizer.yml'
FIXTURE = ROOT / 'tests/data/ci/sanitizer-testlog.txt'
SEPARATOR = '=' * 78
BANNER = '=' * 65


def filter_program():
    text = WORKFLOW.read_text(encoding='utf-8')
    match = re.search(r"if ! awk '\n(.*?)\n\s*' /tmp/qtest-asan-testlog\.txt",
                      text, re.S)
    assert match, 'awk program not found in sanitizer.yml'
    return match.group(1)


def run_filter(awk, testlog):
    return subprocess.run([awk, filter_program(), '-'], input=testlog,
                          capture_output=True, text=True, check=False)


AWKS = [a for a in ('awk', 'mawk', 'gawk') if shutil.which(a)]


def per_awk(test):
    """Run a test body once for each awk implementation present."""
    def run(self):
        for awk in AWKS:
            with self.subTest(awk=awk):
                test(self, awk)
    return run


class FilterTests(unittest.TestCase):

    @per_awk
    def test_prints_the_whole_sanitizer_report(self, awk):
        testlog = FIXTURE.read_text(encoding='utf-8')
        done = run_filter(awk, testlog)
        self.assertEqual(done.returncode, 0, done.stderr)
        out = done.stdout
        self.assertIn('ERROR: AddressSanitizer: heap-use-after-free', out)
        self.assertIn('#0 0x55d0c0a1b2c2 in xbow_lifetime_check', out)
        self.assertIn('SUMMARY: AddressSanitizer', out)
        self.assertIn('==15080==ABORTING', out)
        self.assertIn('sn0-xbow-transport-test', out)
        # The timeout block after it still prints, and the passing
        # block before it does not.
        self.assertIn('sn0-fabric-test', out)
        self.assertNotIn('sn0-example-test', out)

    @per_awk
    def test_banner_alone_does_not_end_a_block(self, awk):
        testlog = '\n'.join([
            '=' * 35 + ' 1/1 ' + '=' * 36, 'test: t', 'result: exit status 1',
            'before', BANNER, 'after the banner', SEPARATOR, ''])
        self.assertIn('after the banner', run_filter(awk, testlog).stdout)

    @per_awk
    def test_report_above_the_tail_window_is_kept(self, awk):
        lines = ['=' * 35 + ' 1/1 ' + '=' * 36, 'test: t',
                 'result: exit status 1', 'start', BANNER,
                 '==1==ERROR: AddressSanitizer: stack-buffer-overflow',
                 '    #0 0x1 in the_culprit file.c:1',
                 'SUMMARY: AddressSanitizer: stack-buffer-overflow',
                 '==1==ABORTING']
        lines += [f'later output {i}' for i in range(400)]
        lines.append(SEPARATOR)
        out = run_filter(awk, '\n'.join(lines) + '\n').stdout
        self.assertIn('in the_culprit', out)
        self.assertIn('later output 399', out)
        self.assertNotIn('later output 100\n', out)

    @per_awk
    def test_undefined_behaviour_report_is_kept(self, awk):
        lines = ['=' * 35 + ' 1/1 ' + '=' * 36, 'test: t',
                 'result: exit status 1', 'x.c:3:4: runtime error: shift',
                 '    #0 0x1 in ubsan_culprit x.c:3',
                 'SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior']
        lines += [f'later output {i}' for i in range(300)] + [SEPARATOR]
        out = run_filter(awk, '\n'.join(lines) + '\n').stdout
        self.assertIn('in ubsan_culprit', out)

    @per_awk
    def test_all_passing_exits_nonzero_for_the_tail_fallback(self, awk):
        testlog = '\n'.join(['=' * 35 + ' 1/1 ' + '=' * 36, 'result: exit status 0',
                             SEPARATOR, ''])
        self.assertEqual(run_filter(awk, testlog).returncode, 3)


if __name__ == '__main__':
    unittest.main()

