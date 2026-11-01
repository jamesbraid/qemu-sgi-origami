#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check scripts/ci/sgi-leak-check.py against a synthetic denylist.

The list here is made up for the test and holds no real pattern: the real
one is private. Each case plants one hit in a throwaway repository and
asserts the check reports it by rule id without printing the planted text.
"""

import base64
import contextlib
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

CHECKER_PATH = Path(__file__).parents[1] / 'ci' / 'sgi-leak-check.py'
spec = importlib.util.spec_from_file_location('sgi_leak_check', CHECKER_PATH)
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)

PLANT = 'zq-planted-token'
LIST = (
    '# sgi-leak-denylist v1 rules=4\n'
    f'T001\tplanted\t{PLANT}\t-\n'
    'T002\tpath\t/scratch/[a-z]+/private\tdocs/allowed.txt\n'
    'T003\tname\t(?i)ZQ-FILENAME\t-\n'
    'T004\tpair\tzq-(?:alpha|beta)-pair\t-\n'
    '@allow\tT004\tzq-beta-pair\n'
)


def run_git(cwd, *args):
    subprocess.run(['git', '-C', str(cwd), '-c', 'user.name=t',
                    '-c', 'user.email=t@example.invalid', *args],
                   check=True, stdout=subprocess.DEVNULL)


class LeakCheck(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name) / 'repo'
        self.root.mkdir()
        self.list = Path(tmp.name) / 'denylist.tsv'
        self.list.write_text(LIST)
        run_git(self.root, 'init', '-q', '-b', 'main')
        self.write('README', 'clean\n')
        self.commit('base')
        self.base = self.rev()
        old = os.getcwd()
        os.chdir(self.root)
        self.addCleanup(os.chdir, old)

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def commit(self, message):
        run_git(self.root, 'add', '-A')
        run_git(self.root, 'commit', '-q', '-m', message)

    def rev(self):
        return subprocess.run(
            ['git', '-C', str(self.root), 'rev-parse', 'HEAD'], check=True,
            stdout=subprocess.PIPE, text=True).stdout.strip()

    def check(self, *args, env=None):
        out, err = io.StringIO(), io.StringIO()
        saved = dict(os.environ)
        os.environ.update(env or {})
        try:
            with contextlib.redirect_stdout(out), \
                    contextlib.redirect_stderr(err):
                rc = checker.main(list(args))
        finally:
            os.environ.clear()
            os.environ.update(saved)
        return rc, out.getvalue() + err.getvalue()

    def diff_check(self, *extra, env=None):
        return self.check('--denylist', str(self.list), '--base', self.base,
                          *extra, env=env)

    def test_clean_range_passes(self):
        self.write('a.txt', 'nothing private\n')
        self.commit('add a')
        rc, out = self.diff_check()
        self.assertEqual(rc, 0, out)
        self.assertIn('clean (4 rules)', out)

    def test_added_line_is_reported_with_file_and_line(self):
        self.write('a.txt', f'one\ntwo {PLANT} three\n')
        self.commit('add a')
        rc, out = self.diff_check()
        self.assertEqual(rc, 1)
        self.assertIn('a.txt:2: T001 (planted)', out)
        self.assertNotIn(PLANT, out)

    def test_only_added_lines_count(self):
        self.write('old.txt', f'{PLANT}\n')
        self.commit('legacy hit')
        self.base = self.rev()
        self.write('old.txt', f'{PLANT}\nnew clean line\n')
        self.commit('touch it')
        rc, out = self.diff_check()
        self.assertEqual(rc, 0, out)

    def test_commit_message_is_scanned(self):
        self.write('a.txt', 'fine\n')
        self.commit(f'a: mention {PLANT}')
        rc, out = self.diff_check()
        self.assertEqual(rc, 1)
        self.assertIn('message line 1: T001', out)
        self.assertNotIn(PLANT, out)

    def test_file_name_is_scanned(self):
        self.write('zq-filename.txt', 'fine\n')
        self.commit('add')
        rc, out = self.diff_check()
        self.assertEqual(rc, 1)
        self.assertIn('zq-filename.txt: T003 (name) in the file name', out)

    def test_pull_request_text_is_scanned(self):
        self.write('a.txt', 'fine\n')
        self.commit('add a')
        event = self.root.parent / 'event.json'
        event.write_text(json.dumps({'pull_request': {
            'title': 'ok', 'body': f'line one\n{PLANT}\n'}}))
        rc, out = self.diff_check('--event', str(event))
        self.assertEqual(rc, 1)
        self.assertIn('pull request body line 2: T001', out)

    def test_permitted_path_glob(self):
        self.write('docs/allowed.txt', '/scratch/x/private\n')
        self.commit('allowed')
        rc, out = self.diff_check()
        self.assertEqual(rc, 0, out)
        self.write('docs/other.txt', '/scratch/x/private\n')
        self.commit('not allowed')
        rc, out = self.diff_check()
        self.assertEqual(rc, 1)
        self.assertIn('docs/other.txt:1: T002', out)
        self.assertNotIn('allowed.txt:1', out)

    def test_tree_mode_finds_legacy_hits(self):
        self.write('old.txt', f'{PLANT}\n')
        self.commit('legacy')
        rc, out = self.check('--denylist', str(self.list), '--tree')
        self.assertEqual(rc, 1)
        self.assertIn('old.txt:1: T001', out)

    def test_unavailable_list_skips_on_a_fork(self):
        rc, out = self.check('--base', self.base,
                             env={'SGI_LEAK_DENYLIST': ''})
        self.assertEqual(rc, 0)
        self.assertIn('SKIPPED', out)

    def test_unavailable_list_fails_when_required(self):
        rc, out = self.check('--base', self.base,
                             env={'SGI_LEAK_DENYLIST': '', 'SGI_LEAK_REQUIRE': '1'})
        self.assertEqual(rc, 2)
        self.assertIn('unavailable', out)

    @staticmethod
    def encode(text):
        return base64.b64encode(zlib.compress(text.encode())).decode()

    def test_secret_delivers_the_list(self):
        self.write('a.txt', f'x {PLANT}\n')
        self.commit('add a')
        rc, out = self.check('--base', self.base,
                             env={'SGI_LEAK_DENYLIST': self.encode(LIST)})
        self.assertEqual(rc, 1)
        self.assertIn('a.txt:1: T001', out)
        self.assertNotIn(PLANT, out)

    def test_garbled_secret_is_an_error_not_a_skip(self):
        rc, out = self.check('--base', self.base,
                             env={'SGI_LEAK_DENYLIST': 'not base64 !!'})
        self.assertEqual(rc, 2)
        self.assertIn('SGI_LEAK_DENYLIST', out)

    def test_allowed_identifier_is_not_a_hit(self):
        self.write('a.txt', 'zq-beta-pair\n')
        self.commit('allowed token')
        rc, out = self.diff_check()
        self.assertEqual(rc, 0, out)
        self.write('b.txt', 'zq-alpha-pair\n')
        self.commit('not allowed')
        rc, out = self.diff_check()
        self.assertEqual(rc, 1)
        self.assertIn('b.txt:1: T004', out)

    def test_truncated_list_is_an_error_not_a_skip(self):
        self.list.write_text(''.join(
            l for l in LIST.splitlines(True) if not l.startswith('T003')))
        rc, out = self.diff_check()
        self.assertEqual(rc, 2)
        self.assertIn('truncated', out)

    def test_bad_pattern_error_does_not_quote_it(self):
        self.list.write_text('# sgi-leak-denylist v1 rules=1\n'
                             'T009\tbad\t(zq-unclosed\t-\n')
        rc, out = self.diff_check()
        self.assertEqual(rc, 2)
        self.assertIn('rule T009', out)
        self.assertNotIn('zq-unclosed', out)


if __name__ == '__main__':
    unittest.main()
