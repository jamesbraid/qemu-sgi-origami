# SPDX-License-Identifier: BSD-3-Clause
"""Contract tests for the nightly guest-asset provisioning script."""

import importlib.util
import os
import runpy
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
provision = runpy.run_path(str(SOURCE / 'scripts/ci/provision-sgi-test-assets.py'))
find_test_module = provision['find_test_module']
selected_assets = provision['selected_assets']


class FindTestModuleTests(unittest.TestCase):
    def tree(self, *relative):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        for name in relative:
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text('')
        return root

    def test_finds_mips64_specific_module(self):
        root = self.tree('mips64/test_sn0_smp.py')
        self.assertEqual(find_test_module(root, 'sn0_smp'),
                         root / 'mips64/test_sn0_smp.py')

    def test_finds_generic_module(self):
        root = self.tree('generic/test_empty_cpu_model.py')
        self.assertEqual(find_test_module(root, 'empty_cpu_model'),
                         root / 'generic/test_empty_cpu_model.py')

    def test_mips64_module_wins_over_generic(self):
        root = self.tree('mips64/test_version.py', 'generic/test_version.py')
        self.assertEqual(find_test_module(root, 'version'),
                         root / 'mips64/test_version.py')

    def test_generic_test_lives_where_the_lookup_expects(self):
        functional = SOURCE / 'tests/functional'
        self.assertEqual(find_test_module(functional, 'empty_cpu_model'),
                         functional / 'generic/test_empty_cpu_model.py')

    def test_missing_module(self):
        self.assertIsNone(find_test_module(self.tree(), 'nothing'))


@unittest.skipUnless(importlib.util.find_spec('pycotap'),
                     'needs the pycotap module from the build tree pyvenv')
class SelectedAssetsTests(unittest.TestCase):
    def setUp(self):
        # qemu_test refuses to import without a build root; nothing here
        # reads from it.
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        saved = os.environ.get('MESON_BUILD_ROOT')
        os.environ['MESON_BUILD_ROOT'] = directory.name
        if saved is None:
            self.addCleanup(os.environ.pop, 'MESON_BUILD_ROOT', None)
        else:
            self.addCleanup(os.environ.__setitem__, 'MESON_BUILD_ROOT', saved)

    def test_generic_target_in_this_tree_resolves(self):
        # Generic tests carry no LocalAsset files; the point is that the
        # lookup finds the module instead of raising ValueError.
        self.assertEqual(
            selected_assets(SOURCE, ['func-mips64-empty_cpu_model']), [])

    def test_unknown_target_is_rejected(self):
        with self.assertRaises(ValueError):
            selected_assets(SOURCE, ['func-mips64-no_such_test'])


if __name__ == '__main__':
    unittest.main()
