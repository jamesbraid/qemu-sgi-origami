#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check the file discovery of scripts/ci/sgi-rust-licences.py."""

import importlib.util
import sys
import tempfile
import json
import unittest
from unittest import mock
from pathlib import Path

PATH = Path(__file__).parents[1] / 'ci' / 'sgi-rust-licences.py'
spec = importlib.util.spec_from_file_location('sgi_rust_licences', PATH)
tool = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = tool
spec.loader.exec_module(tool)


class RustLicences(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def touch(self, name):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('text\n', encoding='utf-8')
        return path

    def test_every_lockfile_is_found_but_not_a_build_directory(self):
        for name in ('rust/a/Cargo.lock', 'rust/b/c/Cargo.lock',
                     'rust/a/target/Cargo.lock', 'other/Cargo.lock'):
            self.touch(name)
        found = [p.relative_to(self.root).as_posix()
                 for p in tool.lockfiles(self.root)]
        self.assertEqual(found, ['rust/a/Cargo.lock', 'rust/b/c/Cargo.lock'])

    def test_git_crate_takes_the_repository_licence(self):
        repo = 'cargo/git/checkouts/iris-1234/abcdef0'
        licence = self.touch(f'{repo}/LICENSE')
        self.touch(f'{repo}/LICENSE-other.txt')
        manifest = self.touch(f'{repo}/crates/iris-wgpu/Cargo.toml')
        package = {'manifest_path': str(manifest),
                   'source': 'git+https://example.org/iris?rev=abcdef0'}
        self.assertEqual(tool.licence_files(package),
                         sorted([licence, self.root / repo / 'LICENSE-other.txt']))

    def test_registry_crate_stays_in_its_directory(self):
        self.touch('registry/src/x/LICENSE')
        manifest = self.touch('registry/src/x/y-1.0/Cargo.toml')
        package = {'manifest_path': str(manifest),
                   'source': 'registry+https://github.com/rust-lang/crates.io-index'}
        self.assertEqual(tool.licence_files(package), [])

    def test_private_exemption_covers_only_our_crates(self):
        iris = tool.IRIS + '?rev=abc#abc'
        ours = self.touch('rust/sgi-gpu-core/Cargo.toml')
        outside = self.touch('vendor/agpl/Cargo.toml')

        def crate(source, manifest):
            return {'source': source, 'manifest_path': str(manifest)}

        self.assertTrue(tool.first_party(crate(iris, ours), self.root))
        self.assertTrue(tool.first_party(crate(None, ours), self.root))
        # The reviewed hole: an AGPL path or git crate marked publish = false.
        self.assertFalse(tool.first_party(crate(None, outside), self.root))
        self.assertFalse(tool.first_party(
            crate('git+https://example.org/agpl?rev=abc#abc', ours), self.root))
        self.assertFalse(tool.first_party(
            crate('git+https://github.com/jamesbraid/iris-evil?rev=abc#abc',
                  ours), self.root))

    def test_private_third_party_crate_fails_the_check(self):
        self.touch('cargo/registry/agpl/LICENSE')
        manifest = self.touch('cargo/registry/agpl/Cargo.toml')
        data = {'workspace_members': [], 'packages': [{
            'id': 'agpl', 'name': 'agpl', 'version': '1.0.0', 'publish': [],
            'source': None, 'manifest_path': str(manifest)}]}
        done = mock.Mock(stdout=json.dumps(data))
        with mock.patch.object(tool.subprocess, 'run', return_value=done):
            with self.assertRaises(SystemExit) as raised:
                tool.first_party_notices(self.root / 'Cargo.toml')
        self.assertIn('agpl 1.0.0', str(raised.exception))


if __name__ == '__main__':
    unittest.main()
