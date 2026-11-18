#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check the IRIX boot matrix script and workflow without booting anything.

The boots themselves run on GitHub (.github/workflows/boot-matrix.yml). These
tests keep the data file, the command line the script builds, the PROM
handling and the workflow's guards from drifting.
"""

import importlib.util
import pathlib
import tempfile
import unittest
from unittest import mock

import yaml

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'scripts/ci/sgi-boot-matrix.py'
WORKFLOW = ROOT / '.github/workflows/boot-matrix.yml'
RELEASE = ROOT / '.github/workflows/release.yml'

spec = importlib.util.spec_from_file_location('sgi_boot_matrix', SCRIPT)
bm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bm)


class DataTests(unittest.TestCase):
    def setUp(self):
        self.data = bm.load_data()

    def test_the_data_file_is_consistent(self):
        self.assertEqual(bm.check_data(self.data), [])

    def test_linux_boots_every_machine_and_the_others_one_per_family(self):
        ids = [b['id'] for b in self.data['boots']]
        self.assertEqual(
            [b['id'] for b in bm.matrix(self.data, 'linux')['include']], ids)
        for os_name in ('macos', 'windows'):
            got = bm.matrix(self.data, os_name)['include']
            self.assertEqual(sorted(b['family'] for b in got),
                             sorted(f['id'] for f in self.data['families']))

    def test_octane_is_skipped_with_a_reason(self):
        skipped = {s['id']: s for s in self.data['skipped']}
        self.assertIn('octane', skipped)
        self.assertNotIn('octane', [b['id'] for b in self.data['boots']])
        self.assertIn('IP30', skipped['octane']['reason'])

    def test_every_pinned_object_is_hashed_and_on_the_public_host(self):
        self.assertTrue(self.data['dist'].startswith('https://'))
        for name, obj in self.data['objects'].items():
            self.assertRegex(obj['sha256'], r'^[0-9a-f]{64}$', name)

    def test_a_bad_data_file_is_reported(self):
        data = bm.load_data()
        data['boots'][0]['hinv'] = []
        data['boots'].append(dict(data['boots'][0]))
        problems = bm.check_data(data)
        self.assertTrue(any('no hinv expectations' in p for p in problems))
        self.assertTrue(any('duplicate boot' in p for p in problems))


class ScriptTests(unittest.TestCase):
    def test_sha256sums_are_parsed(self):
        text = ('0' * 64 + '  prom/a.img\n' + 'A' * 64 + ' *irix/b.chd\n'
                'not a line\n')
        self.assertEqual(bm.parse_sha256sums(text),
                         {'prom/a.img': '0' * 64, 'irix/b.chd': 'a' * 64})

    def test_the_o2_installer_header_is_stripped_and_padded(self):
        payload = bytes(range(200))
        header = bytearray(256)
        header[:4] = b'PROM'
        header[12:16] = len(payload).to_bytes(4, 'big')
        with tempfile.TemporaryDirectory() as d:
            src, dest = pathlib.Path(d, 'in'), pathlib.Path(d, 'out')
            src.write_bytes(bytes(header) + payload)
            bm.o2_flash_from_installer(src, dest)
            flash = dest.read_bytes()
            self.assertEqual(len(flash), 524288)
            self.assertEqual(flash[:200], payload)
            self.assertEqual(set(flash[200:]), {0xff})
            src.write_bytes(b'XXXX' + bytes(300))
            with self.assertRaises(RuntimeError):
                bm.o2_flash_from_installer(src, dest)

    def test_the_command_line_binds_every_store_and_the_chd_overlay(self):
        data = bm.load_data()
        boot = data['boots'][0]
        stores = {'node0-flash': pathlib.Path('/s/a,b/node0-flash.raw'),
                  'nvram0': pathlib.Path('/s/nvram0.raw')}
        args = bm.qemu_arguments(boot, stores, pathlib.Path('/d/x.chd'), 1234,
                                 pathlib.Path('/a/bin'))
        machine = args[args.index('-M') + 1]
        self.assertIn('node0-flash=node0-flash', machine)
        self.assertIn('nvram0=nvram0', machine)
        blockdevs = [args[i + 1] for i, a in enumerate(args)
                     if a == '-blockdev']
        self.assertEqual(len(blockdevs), 2)
        self.assertIn('/s/a,,b/node0-flash.raw', blockdevs[0])
        drive = args[args.index('-drive') + 1]
        self.assertIn('format=chd', drive)
        self.assertIn('overlay=on', drive)
        self.assertEqual(args[-2:], ['-serial',
                                     'tcp:127.0.0.1:1234,server=on,wait=off'])

    def test_serial_lines_before_the_console_have_no_backend(self):
        boot = dict(bm.load_data()['boots'][0], console_line=2)
        args = bm.qemu_arguments(boot, {}, pathlib.Path('d.chd'), 1,
                                 pathlib.Path('/a/bin'))
        self.assertEqual(args.count('-serial'), 3)
        self.assertEqual(args[args.index('-serial') + 1], 'none')

    def test_hinv_expectations_are_checked_one_by_one(self):
        got = bm.hinv_checks('1 250 MHZ IP27 Processor\nMain memory: 256',
                             [r'IP27 Processor', r'Main memory: 512'])
        self.assertEqual(got, {r'IP27 Processor': True,
                               r'Main memory: 512': False})


class FakeProc:
    returncode = None

    def poll(self):
        return None

    def kill(self):
        self.returncode = -9

    def wait(self, timeout=None):
        return self.returncode


class ExitStatusTests(unittest.TestCase):
    """The job is green only if the boot passed, whatever stage it failed at."""

    def run_boot(self, login):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            disk, prom = d / 'disk.chd', d / 'prom.img'
            disk.write_bytes(b'x')
            prom.write_bytes(b'x')
            objs = {'ip27-disk': disk, 'ip27-prom': prom}
            args = bm.argparse.Namespace(
                archive='unused', id='origin200', work=str(d / 'work'),
                cache=None, results=str(d / 'results.json'),
                login_timeout=1)

            def init(cmd, check):
                stores = pathlib.Path(cmd[-1])
                stores.mkdir()
                (stores / 'nvram0.raw').write_bytes(b'')

            with mock.patch.object(bm, 'fetch_objects', return_value=objs), \
                    mock.patch.object(bm, 'extract',
                                      return_value=d / 'bin'), \
                    mock.patch.object(bm.subprocess, 'run', side_effect=init), \
                    mock.patch.object(bm.subprocess, 'Popen',
                                      return_value=FakeProc()), \
                    mock.patch.object(bm, 'Console'), \
                    mock.patch.object(bm, 'login', side_effect=login):
                try:
                    bm.boot_one(args)
                    code = 0
                except SystemExit as e:
                    code = e.code
            return code, bm.json.loads((d / 'results.json').read_text())

    def test_a_login_that_times_out_fails_the_job_and_is_recorded(self):
        def login(*_):
            raise bm.Timeout('no login')
        code, results = self.run_boot(login)
        self.assertEqual(code, 1)
        self.assertEqual(results['status'], 'fail')
        self.assertFalse(results['checks']['login']['ok'])

    def test_a_console_that_closes_fails_the_job(self):
        def login(*_):
            raise EOFError('console closed')
        code, _ = self.run_boot(login)
        self.assertEqual(code, 1)


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.text = WORKFLOW.read_text()
        self.wf = yaml.safe_load(self.text)

    def test_every_job_runs_on_github_only(self):
        for name, job in self.wf['jobs'].items():
            self.assertIn("github.server_url == 'https://github.com'",
                          job['if'], name)

    @unittest.skipUnless(RELEASE.exists(), 'release.yml has not landed')
    def test_it_follows_the_release_workflow_by_name_and_does_not_edit_it(self):
        release = yaml.safe_load(RELEASE.read_text())
        triggers = self.wf.get('on', self.wf.get(True))
        self.assertEqual(triggers['workflow_run']['workflows'],
                         [release['name']])
        self.assertIn('schedule', triggers)
        self.assertIn('workflow_dispatch', triggers)

    def test_job_names_say_what_they_check(self):
        self.assertIn('hinv', self.wf['name'] + ' '.join(
            j['name'] for j in self.wf['jobs'].values()))
        self.assertIn('IRIX', self.wf['name'])
        for os_name in ('linux', 'macos', 'windows'):
            self.assertIn('IRIX boots to login and hinv matches',
                          self.wf['jobs'][os_name]['name'])

    def test_the_os_jobs_may_read_another_run_s_artifacts(self):
        for os_name in ('linux', 'macos', 'windows'):
            perms = self.wf['jobs'][os_name]['permissions']
            self.assertEqual(perms.get('actions'), 'read', os_name)

    def test_only_release_tag_builds_start_the_workflow_run_path(self):
        cond = self.wf['jobs']['plan']['if']
        self.assertIn("startsWith(github.event.workflow_run.head_branch, "
                      "'sgi-origami-')", cond)
        self.assertIn("workflow_run.event == 'push'", cond)

    def test_the_hinv_negative_pattern_is_honoured(self):
        got = bm.hinv_checks('Iris Audio Processor: x\n',
                             ['!^Iris Audio Processor:', 'Audio'])
        self.assertEqual(got, {'!^Iris Audio Processor:': False,
                               'Audio': True})

    @unittest.skipUnless(RELEASE.exists(), 'release.yml has not landed')
    def test_each_os_downloads_the_archive_release_yml_uploads(self):
        release = RELEASE.read_text()
        # release.yml names the Linux and macOS archives from a matrix arch.
        for artifact, uploaded in (
                ('archive-linux-x86_64', 'archive-linux-${{ matrix.arch }}'),
                ('archive-macos-arm64', 'archive-macos-${{ matrix.arch }}'),
                ('archive-windows-x86_64', 'archive-windows-x86_64')):
            self.assertIn(uploaded, release)
            self.assertIn(artifact, self.text)


if __name__ == '__main__':
    unittest.main()
