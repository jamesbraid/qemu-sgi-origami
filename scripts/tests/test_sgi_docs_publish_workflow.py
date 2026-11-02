#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check the structure of the workflow that publishes the manual.

The workflow holds a deploy credential and publishes without review, so a
later edit must not quietly drop the rules that keep it safe: it skips on
a fork and on a repository without the credentials, it never prints
them, it builds with warnings as errors, and it points the page-source
links at the mirror instead of upstream.
"""

import pathlib
import re
import unittest

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
PATH = ROOT / '.github/workflows/docs-publish.yml'
TEXT = PATH.read_text()
WF = yaml.safe_load(TEXT)
JOB = WF['jobs']['publish']
STEPS = {s['name']: s for s in JOB['steps'] if 'name' in s}
GATED = "steps.creds.outputs.present == 'true'"


class DocsPublishWorkflowTests(unittest.TestCase):
    def test_runs_on_trunk_pushes_and_by_hand(self):
        # PyYAML reads the bare key "on" as True.
        triggers = WF.get('on', WF.get(True))
        self.assertEqual(triggers['push']['branches'], ['sgi-origami'])
        self.assertIn('docs/**', triggers['push']['paths'])
        self.assertIn('qapi/**', triggers['push']['paths'])
        self.assertIn('workflow_dispatch', triggers)
        # Nightly, to catch inputs the path list misses.
        self.assertTrue(triggers['schedule'][0]['cron'])
        self.assertNotIn('pull_request', triggers)

    def test_job_skips_forks_the_mirror_and_other_branches(self):
        cond = JOB['if']
        self.assertIn("github.repository == 'jamesb/qemu'", cond)
        self.assertIn("github.server_url != 'https://github.com'", cond)
        self.assertIn("github.ref == 'refs/heads/sgi-origami'", cond)

    def test_missing_credentials_skip_every_later_step(self):
        first = JOB['steps'][0]
        self.assertEqual(first['id'], 'creds')
        self.assertIn('set +x', first['run'])
        self.assertIn('-n "$CLOUDFLARE_API_TOKEN"', first['run'])
        self.assertIn('-n "$CLOUDFLARE_ACCOUNT_ID"', first['run'])
        self.assertIn('::notice::', first['run'])
        self.assertNotIn('exit 1', first['run'])
        for step in JOB['steps'][1:]:
            self.assertEqual(step.get('if'), GATED, step)

    def test_credentials_come_from_secrets_and_only_two_steps_see_them(self):
        holders = []
        for step in JOB['steps']:
            env = step.get('env') or {}
            for name in ('CLOUDFLARE_API_TOKEN', 'CLOUDFLARE_ACCOUNT_ID'):
                if name in env:
                    self.assertEqual(env[name], '${{ secrets.%s }}' % name)
                    holders.append(step.get('name'))
        self.assertEqual(sorted(set(holders)),
                         ['check for deploy credentials', 'deploy'])
        for scope in (WF, JOB):
            self.assertFalse(
                [k for k in scope.get('env', {}) if 'CLOUDFLARE' in k])

    def test_no_step_prints_or_traces_the_credentials(self):
        self.assertIsNone(
            re.search(r'(echo|printf|cat)[^\n]*CLOUDFLARE_API_TOKEN', TEXT))
        self.assertNotRegex(TEXT, r'(?m)^\s*(set -x|set -[a-z]*x)')
        self.assertIn('set +x', STEPS['deploy']['run'])

    def test_build_is_pinned_and_fails_on_warnings(self):
        install = STEPS['install Sphinx']['run']
        self.assertIn('sphinx==7.2.6', install)
        self.assertIn('sphinx_rtd_theme==2.0.0', install)
        build = STEPS['build the manual']['run']
        self.assertIn('-W --keep-going', build)
        self.assertIn('-b html', build)
        self.assertIn('-Dversion=', build)
        self.assertIn('-Drelease=', build)

    def test_source_links_point_at_the_mirror_not_upstream(self):
        prefix = JOB['env']['SOURCE_URL_PREFIX']
        self.assertTrue(prefix.startswith('https://github.com/'))
        self.assertTrue(prefix.endswith('/blob/sgi-origami/docs/'))
        self.assertNotIn('gitlab', prefix)
        build = STEPS['build the manual']['run']
        self.assertIn('-A source_url_prefix="$SOURCE_URL_PREFIX"', build)

    def test_deploy_pins_the_tool_and_names_the_production_branch(self):
        run = STEPS['deploy']['run']
        self.assertRegex(run, r'wrangler@\d+\.\d+\.\d+ pages deploy')
        self.assertIn('--project-name=origami-qemu-docs', run)
        self.assertIn('--branch=main', run)

    def test_checkout_keeps_no_credentials(self):
        checkout = [s for s in JOB['steps']
                    if str(s.get('uses', '')).startswith('actions/checkout')]
        self.assertEqual(len(checkout), 1)
        self.assertIs(checkout[0]['with']['persist-credentials'], False)

    def test_deploys_queue_instead_of_interrupting(self):
        self.assertIs(WF['concurrency']['cancel-in-progress'], False)


if __name__ == '__main__':
    unittest.main()
