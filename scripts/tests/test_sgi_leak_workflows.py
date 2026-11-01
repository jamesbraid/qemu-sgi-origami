#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check that the merge queue names no account or host in its workflow.

The tests read the workflow file, so a later edit cannot put a literal
name back.
"""

import pathlib
import re
import unittest

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / '.github/workflows'


def load(name):
    text = (WORKFLOWS / name).read_text()
    return text, yaml.safe_load(text)


def steps_running(job, needle):
    return [s for s in job.get('steps', [])
            if needle in (s.get('run') or '')]


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
