# SPDX-License-Identifier: GPL-2.0-or-later

import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

SCRIPT = Path(__file__).parents[1] / 'archive-source.sh'
SUBPROJECTS = (
    'anyhow-1-rs',
    'attrs-0.2-rs',
    'berkeley-softfloat-3',
    'berkeley-testfloat-3',
    'bitfield-struct-0.13-rs',
    'foreign-0.4-rs',
    'glib-sys-0.21-rs',
    'keycodemapdb',
    'libc-0.2-rs',
    'libslirp',
    'libvfio-user',
    'probe-0.5-rs',
    'proc-macro2-1-rs',
    'quote-1-rs',
    'syn-2-rs',
    'unicode-ident-1-rs',
)


class ArchiveSourceTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.tree = self.root / 'source'
        (self.tree / 'subprojects').mkdir(parents=True)
        (self.tree / 'tracked.txt').write_text('tracked source\n')
        self._write_tool('git', '''#!/bin/sh
case "$1" in
  diff-index) exit 0 ;;
  archive) exec tar -cf - tracked.txt ;;
  *) exit 2 ;;
esac
''')
        self._write_tool('meson', '#!/bin/sh\nexit 0\n')
        self.env = os.environ.copy()
        self.env['PATH'] = f"{self.bin}{os.pathsep}{self.env['PATH']}"
        self.output = self.root / 'source.tar'

    def tearDown(self):
        self.tempdir.cleanup()

    def _write_tool(self, name, body):
        path = self.bin / name
        path.write_text(body)
        path.chmod(0o755)

    def _add_subprojects(self):
        expected = set()
        for name in SUBPROJECTS:
            directory = f'dir-{name}'
            expected.add(directory)
            (self.tree / 'subprojects' / f'{name}.wrap').write_text(
                f'[wrap-file]\ndirectory = {directory}\n')
            project = self.tree / 'subprojects' / directory
            (project / '.git').mkdir(parents=True)
            (project / '.git' / 'config').write_text('internal\n')
            (project / 'payload.txt').write_text(f'{name}\n')
        unrelated = self.tree / 'subprojects' / 'unrelated'
        unrelated.mkdir()
        (unrelated / 'payload.txt').write_text('not declared\n')
        return expected

    def _run_archive(self):
        return subprocess.run(
            [str(SCRIPT), str(self.output)], cwd=self.tree, env=self.env,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=False)

    def test_missing_wrap_fails_without_appending_subprojects(self):
        unrelated = self.tree / 'subprojects' / 'unrelated'
        unrelated.mkdir()
        (unrelated / 'payload.txt').write_text('not declared\n')

        result = self._run_archive()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn('failed to resolve subproject directory', result.stderr)
        with tarfile.open(self.output) as archive:
            self.assertEqual(archive.getnames(), ['tracked.txt'])

    def test_archives_declared_subprojects_only_without_git_metadata(self):
        expected = self._add_subprojects()

        result = self._run_archive()

        self.assertEqual(result.returncode, 0, result.stderr)
        with tarfile.open(self.output) as archive:
            names = set(archive.getnames())
        self.assertIn('tracked.txt', names)
        archived_dirs = {
            name.split('/')[1] for name in names if name.startswith('subprojects/')
        }
        self.assertEqual(archived_dirs, expected)
        self.assertFalse(any('/.git/' in name for name in names))
        self.assertFalse(any(name.startswith('subprojects/unrelated/')
                             for name in names))


if __name__ == '__main__':
    unittest.main()
