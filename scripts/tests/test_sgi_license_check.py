#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check scripts/ci/sgi-license-check.py on synthetic trees and this one."""

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

CHECKER_PATH = Path(__file__).parents[1] / 'ci' / 'sgi-license-check.py'
spec = importlib.util.spec_from_file_location('sgi_license_check',
                                              CHECKER_PATH)
checker = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = checker
spec.loader.exec_module(checker)

BSD = '/* SPDX-License-Identifier: BSD-3-Clause */\n'


class SgiLicenseCheck(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def tree(self, files, listed):
        for name, text in files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
        (self.root / checker.LIST).write_text(
            ''.join(f'{name}\n' for name in listed), encoding='utf-8')

    def findings(self):
        return dict(checker.check(self.root))

    def test_clean_tree(self):
        self.tree({'hw/misc/sgi-a.c': BSD, 'hw/misc/upstream.c': 'x\n'},
                  ['hw/misc/sgi-a.c'])
        self.assertEqual(self.findings(), {})

    def test_unlisted_file(self):
        self.tree({'hw/misc/sgi-a.c': BSD, 'hw/misc/sgi-b.c': BSD},
                  ['hw/misc/sgi-a.c'])
        found = self.findings()
        self.assertEqual(list(found), ['hw/misc/sgi-b.c'])
        self.assertIn('not listed', found['hw/misc/sgi-b.c'])

    def test_missing_listed_file(self):
        self.tree({'hw/misc/sgi-a.c': BSD},
                  ['hw/misc/sgi-a.c', 'hw/misc/sgi-gone.c'])
        found = self.findings()
        self.assertEqual(list(found), ['hw/misc/sgi-gone.c'])
        self.assertIn('does not exist', found['hw/misc/sgi-gone.c'])

    def test_wrong_licence(self):
        self.tree({'hw/misc/sgi-a.c':
                   '/* SPDX-License-Identifier: GPL-2.0-or-later */\n',
                   'hw/1wire/core.c': 'no header\n'},
                  ['hw/misc/sgi-a.c', 'hw/1wire/core.c'])
        found = self.findings()
        self.assertIn('GPL-2.0-or-later', found['hw/misc/sgi-a.c'])
        self.assertIn('no SPDX', found['hw/1wire/core.c'])

    def test_wrong_licence_on_unlisted_match(self):
        self.tree({'hw/misc/sgi-a.c':
                   '/* SPDX-License-Identifier: GPL-2.0-or-later */\n'}, [])
        self.assertEqual(len(checker.check(self.root)), 2)

    def test_exempt_data_file(self):
        self.tree({'scripts/tests/system-model-fixtures/a/resolved.json': '{}',
                   'scripts/tests/system-model-fixtures/a/resolved.sha256':
                   'abc\n'},
                  ['scripts/tests/system-model-fixtures/a/resolved.json',
                   'scripts/tests/system-model-fixtures/a/resolved.sha256'])
        self.assertEqual(self.findings(), {})

    def test_upstream_file_in_sgi_directory(self):
        self.tree({'tests/functional/mips64/test_malta.py':
                   '# SPDX-License-Identifier: GPL-2.0-or-later\n'}, [])
        self.assertEqual(self.findings(), {})

    def test_spdx_after_a_long_comment(self):
        self.tree({'hw/misc/sgi-a.c':
                   '/*\n' + ' * text\n' * 30 + ' * SPDX-License-Identifier: '
                   'BSD-3-Clause\n */\n'}, ['hw/misc/sgi-a.c'])
        self.assertEqual(self.findings(), {})

    def test_this_tree(self):
        self.assertEqual(checker.check(checker.SOURCE), [])


if __name__ == '__main__':
    unittest.main()
