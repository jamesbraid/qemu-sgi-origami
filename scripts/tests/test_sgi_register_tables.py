#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check scripts/ci/sgi-register-tables.py on small pages and this tree.

Each rule has a page that passes and a page with one marker or cell changed
that must be reported, so a marker cannot turn into a silent skip.
"""

import importlib.machinery
import importlib.util
import tempfile
import unittest
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2]
_LOADER = importlib.machinery.SourceFileLoader(
    'sgi_register_tables', str(SOURCE / 'scripts/ci/sgi-register-tables.py'))
_SPEC = importlib.util.spec_from_loader('sgi_register_tables', _LOADER)
tables = importlib.util.module_from_spec(_SPEC)
_LOADER.exec_module(tables)

MARKER = ('.. sgi-registers: base=0x1000 width=64\n'
          '   qemu="-M none -device foo"\n')

PAGE = '''.. SPDX-License-Identifier: BSD-3-Clause

Title
=====

%s
.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
   * - ``0x00``
     - Control
     - ``0x20000001``
   * - ``0x08``, ``0x10``
     - Two
     - 0
   * - ``0x20``-``0x28``
     - Range
     - ``0``; ``0x8118`` on revision F
   * - ``0x30``
     - Write only
     - n/a
   * - ``0x38``
     - Prose
     - per machine
'''


def scan(text, marker=MARKER, skip=None, expect=None):
    if skip:
        marker = marker.replace('width=64', f'width=64 skip={skip}')
    if expect:
        marker = marker.replace('width=64', f'width=64 expect={expect}')
    with tempfile.TemporaryDirectory() as root:
        pages = Path(root) / tables.PAGES / 'hardware'
        pages.mkdir(parents=True)
        (pages / 'x.rst').write_text(text % marker, encoding='utf-8')
        return tables.scan(root)


class RegisterTablesTest(unittest.TestCase):
    def test_values_and_skips(self):
        cases, annotated, other, errors = scan(PAGE, skip='0x38')
        self.assertEqual(errors, [])
        (case,) = cases
        regs = {r['offset']: r for r in case['registers']}
        self.assertEqual(sorted(regs, key=lambda o: int(o, 16)),
                         ['0x0', '0x8', '0x10', '0x20', '0x28'])
        self.assertEqual(regs['0x0']['address'], '0x1000')
        self.assertEqual(regs['0x0']['expect'], '0x20000001')
        self.assertEqual(regs['0x28']['expect'], '0x0')
        self.assertEqual([s['offset'] for s in case['skipped']],
                         ['0x30', '0x38'])
        self.assertEqual(len(annotated), 1)
        self.assertEqual(other, [])

    def test_variant_picks_the_labelled_value(self):
        marker = MARKER.replace('width=64', 'width=64 variant="revision F"')
        cases, _, _, errors = scan(PAGE, marker, skip='0x38')
        self.assertEqual(errors, [])
        regs = {r['offset']: r['expect'] for r in cases[0]['registers']}
        self.assertEqual(regs['0x20'], '0x8118')
        self.assertEqual(regs['0x0'], '0x20000001')

    def test_unknown_variant_is_an_error(self):
        marker = MARKER.replace('width=64', 'width=64 variant="revision Z"')
        self.assertIn('matches no Reset cell',
                      '\n'.join(scan(PAGE, marker, skip='0x38')[3]))

    def test_prose_reset_needs_skip(self):
        errors = scan(PAGE)[3]
        self.assertEqual(len(errors), 1)
        self.assertIn('x.rst:', errors[0])
        self.assertIn('name the offset in skip=', errors[0])

    def test_unknown_key_is_an_error(self):
        marker = MARKER.replace('width=64', 'width=64 colour=red')
        self.assertIn('unknown marker word',
                      '\n'.join(scan(PAGE, marker, skip='0x38')[3]))

    def test_missing_qemu_is_an_error(self):
        marker = '.. sgi-registers: base=0x1000 width=64\n'
        self.assertIn('no qemu=',
                      '\n'.join(scan(PAGE, marker, skip='0x38')[3]))

    def test_marker_away_from_a_table_is_an_error(self):
        text = PAGE.replace('.. list-table::', 'Prose between.\n\n'
                            '.. list-table::', 1)
        errors = scan(text, skip='0x38')[3]
        self.assertTrue(any('not directly above' in e for e in errors))

    def test_unmarked_table_is_counted_not_checked(self):
        cases, annotated, other, errors = scan(PAGE, marker='')
        self.assertEqual((cases, annotated, errors), ([], [], []))
        self.assertEqual(len(other), 1)

    def test_width_column_overrides_the_marker(self):
        text = PAGE.replace('     - Reset\n', '     - Width\n     - Reset\n')
        out = []
        for row in text.split('\n'):
            out.append(row)
            if row.startswith('     - ') and row.strip() in (
                    '- Control', '- Two', '- Range', '- Write only',
                    '- Prose'):
                out.append('     - 32')
        cases, _, _, errors = scan('\n'.join(out), skip='0x38')
        self.assertEqual(errors, [])
        self.assertEqual({r['width'] for r in cases[0]['registers']}, {32})

    def test_stale_skip_is_an_error(self):
        errors = scan(PAGE, skip='0x38,0x400')[3]
        self.assertIn('names no row at offset 0x400', '\n'.join(errors))

    def test_skipping_a_number_needs_a_reason(self):
        errors = scan(PAGE, skip='0x38,0x0')[3]
        self.assertIn('give skip= a reason', '\n'.join(errors))
        cases, _, _, errors = scan(PAGE, skip='0x38,0x0:poll-flags')
        self.assertEqual(errors, [])
        reasons = {s['offset']: s['reason'] for s in cases[0]['skipped']}
        self.assertEqual(reasons['0x0'], 'poll-flags')

    def test_expect_pins_the_model_value(self):
        cases, _, _, errors = scan(PAGE, skip='0x38', expect='0x8:0x1400')
        self.assertEqual(errors, [])
        regs = {r['offset']: r for r in cases[0]['registers']}
        self.assertEqual(regs['0x8']['expect'], '0x1400')
        self.assertIn('pinned', regs['0x8']['note'])
        self.assertNotIn('note', regs['0x0'])

    def test_expect_that_repeats_the_page_or_misses_is_an_error(self):
        errors = scan(PAGE, skip='0x38', expect='0x8:0')[3]
        self.assertIn('repeats the page value', '\n'.join(errors))
        errors = scan(PAGE, skip='0x38', expect='0x400:1')[3]
        self.assertIn('names no row', '\n'.join(errors))
        errors = scan(PAGE, skip='0x8,0x38', expect='0x8:1')[3]
        self.assertIn('both skip= and expect=', '\n'.join(errors))

    def test_cell_with_only_another_variant_is_an_error(self):
        text = PAGE.replace('``0x20000001``', '``0x1`` on rev 4')
        errors = scan(text, skip='0x38')[3]
        self.assertEqual(len(errors), 1)
        self.assertIn('x.rst:', errors[0])
        self.assertIn('no value for variant', errors[0])

    def test_marker_on_a_table_without_reset_is_an_error(self):
        text = PAGE.replace('     - Reset\n', '     - Value\n')
        errors = scan(text, skip='0x38')[3]
        self.assertTrue(any('markers but 0 tables' in e for e in errors))

    def test_slash_pairs_each_offset_with_its_width(self):
        text = PAGE.replace('     - Reset\n', '     - Width\n     - Reset\n')
        out = []
        for row in text.split('\n'):
            out.append(row)
            if row.strip() == '- Two':
                out[-2] = '   * - ``0x08`` / ``0x0c``'
                out.append('     - 64 / 32')
            elif row.startswith('     - ') and row.strip() in (
                    '- Control', '- Range', '- Write only', '- Prose'):
                out.append('     - 64')
        cases, _, _, errors = scan('\n'.join(out), skip='0x38')
        self.assertEqual(errors, [])
        regs = {r['offset']: r['width'] for r in cases[0]['registers']}
        self.assertEqual((regs['0x8'], regs['0xc']), (64, 32))

    def test_expect_may_pin_a_cell_with_no_plain_value(self):
        text = PAGE.replace('``0x20000001``', '1 on read')
        cases, _, _, errors = scan(text, skip='0x38', expect='0x0:1')
        self.assertEqual(errors, [])
        regs = {r['offset']: r['expect'] for r in cases[0]['registers']}
        self.assertEqual(regs['0x0'], '0x1')

    def test_this_tree(self):
        cases, annotated, other, errors = tables.scan(SOURCE)
        self.assertEqual(errors, [])
        self.assertGreaterEqual(len(annotated), 3)
        for case in cases:
            self.assertTrue(case['registers'], case['id'])


if __name__ == '__main__':
    unittest.main()
