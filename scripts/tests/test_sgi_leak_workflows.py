#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check that the workflows run the private identifier scan where it must
run, and that the merge queue names no account or host in its file.

The scan itself is tested in test_sgi_leak_check.py. These tests read the
workflow files, so a later edit cannot quietly drop the whole-tree scan
from the places the repository relies on it, or turn the pull-request
check from a diff scan into a slow tree scan.
"""

import pathlib
import re
import unittest

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / '.github/workflows'
SCAN = 'scripts/ci/sgi-leak-check.py'


def load(name):
    text = (WORKFLOWS / name).read_text()
    return text, yaml.safe_load(text)


def steps_running(job, needle):
    return [s for s in job.get('steps', [])
            if needle in (s.get('run') or '')]


class TreeScanWorkflowTests(unittest.TestCase):
    def test_scan_runs_on_trunk_pushes_and_nightly(self):
        _text, wf = load('leak-scan.yml')
        # PyYAML reads the bare key "on" as True.
        triggers = wf.get('on', wf.get(True))
        self.assertEqual(triggers['push']['branches'], ['sgi-origami'])
        self.assertTrue(triggers['schedule'][0]['cron'])
        self.assertIn('workflow_dispatch', triggers)

    def test_scan_is_the_whole_tree_and_fails_closed_on_the_main_repo(self):
        _text, wf = load('leak-scan.yml')
        steps = steps_running(wf['jobs']['scan-tree'], f'{SCAN} --tree')
        self.assertEqual(len(steps), 1)
        env = steps[0]['env']
        self.assertEqual(env['SGI_LEAK_DENYLIST'],
                         '${{ secrets.SGI_LEAK_DENYLIST }}')
        self.assertIn("github.repository == 'jamesb/qemu'",
                      env['SGI_LEAK_REQUIRE'])
        self.assertIn('set +x', steps[0]['run'])

    def test_full_run_scans_the_tree_before_a_land(self):
        _text, wf = load('build.yml')
        job = wf['jobs']['full-part']
        steps = steps_running(job, f'{SCAN} --tree')
        self.assertEqual(len(steps), 1)
        self.assertIn("startsWith(matrix.part, 'functional')", steps[0]['if'])
        env = steps[0]['env']
        self.assertEqual(env['SGI_LEAK_DENYLIST'],
                         '${{ secrets.SGI_LEAK_DENYLIST }}')
        self.assertIn("github.repository == 'jamesb/qemu'",
                      env['SGI_LEAK_REQUIRE'])

    def test_pull_request_check_stays_a_diff_scan(self):
        text, wf = load('build.yml')
        self.assertEqual(wf['name'], 'build and register suite')
        job = wf['jobs']['build-and-qtest']
        steps = steps_running(job, SCAN)
        self.assertEqual(len(steps), 1)
        self.assertIn('--base', steps[0]['run'])
        self.assertNotIn('--tree', steps[0]['run'])
        self.assertEqual(steps[0]['if'],
                         "${{ github.event_name == 'pull_request' }}")

    def test_no_workflow_prints_the_list(self):
        for path in WORKFLOWS.glob('*.yml'):
            text = path.read_text()
            self.assertIsNone(
                re.search(r'(echo|printf|cat)[^\n]*SGI_LEAK_DENYLIST', text),
                path.name)
            if '--tree' in text:
                self.assertIn('set +x', text, path.name)


class MergeQueueNamesTests(unittest.TestCase):
    def test_committer_and_host_come_from_the_variable_or_the_forge(self):
        text, wf = load('merge-queue.yml')
        steps = steps_running(wf['jobs']['queue'], 'GIT_CONFIG_VALUE_1')
        self.assertEqual(len(steps), 1)
        run = steps[0]['run']
        self.assertEqual(steps[0]['env']['COMMITTER_LOGIN'],
                         '${{ vars.CI_BOT_USER }}')
        self.assertIn('GIT_CONFIG_VALUE_1=$bot', run)
        self.assertIn('GIT_CONFIG_VALUE_2=$bot@noreply.$host', run)
        self.assertIn('"$FORGE/api/v1/user"', run)
        self.assertIsNone(re.search(r'GIT_CONFIG_VALUE_[12]=[A-Za-z]', run))
        # Whitespace is stripped from both sources, and an empty, None or
        # null login stops the step with the message naming the variable.
        self.assertEqual(run.count("tr -d '[:space:]'"), 2)
        self.assertRegex(run, r"''\|None\|null\)")
        self.assertIn('set the CI_BOT_USER variable', run)

    def test_login_check_behaves_as_the_workflow_runs_it(self):
        import os
        import subprocess
        import tempfile
        _text, wf = load('merge-queue.yml')
        step = steps_running(wf['jobs']['queue'], 'GIT_CONFIG_VALUE_1')[0]
        run = step['run'].split('          rm -rf')[0].split('auth=$(')[0]
        with tempfile.TemporaryDirectory() as tmp:
            curl = pathlib.Path(tmp) / 'curl'
            curl.write_text('#!/bin/sh\nprintf \'%s\' "$FAKE_API"\n')
            curl.chmod(0o755)

            def go(login, api=''):
                env = dict(os.environ, PATH=tmp + ':' + os.environ['PATH'],
                           COMMITTER_LOGIN=login, FAKE_API=api,
                           FORGE='https://forge.example', TOKEN='t')
                done = subprocess.run(
                    ['bash', '-ec', run + '\necho "$bot"'], env=env,
                    capture_output=True, text=True)
                return done.returncode, done.stdout

            self.assertEqual(go(' named\n'), (0, 'named\n'))
            self.assertEqual(go('', '{"login": "viaapi"}'), (0, 'viaapi\n'))
            for api in ('{"login": null}', '{"login": ""}', '{}', 'junk'):
                rc, out = go('', api)
                self.assertNotEqual(rc, 0, api)
            rc, out = go(' \n', '{"login": null}')
            self.assertNotEqual(rc, 0)
            self.assertIn('set the CI_BOT_USER variable', out)


if __name__ == '__main__':
    unittest.main()
