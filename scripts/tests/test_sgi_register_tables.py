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

    def test_empty_cells_stay_cells(self):
        # A bare "-" is an empty cell; it must not fold into the cell
        # before it and shift the Reset column.
        text = PAGE.replace(
            '''   * - ``0x38``
     - Prose
     - per machine
''', '''   * - ``0x38``
     - Gap
     -
     -
''')
        text = text.replace('     - Reset\n', '     - Width\n     - Reset\n')
        text = text.replace('     - Control\n', '     - Control\n     - 64\n')
        text = text.replace('     - Two\n', '     - Two\n     - 64\n')
        text = text.replace('     - Range\n', '     - Range\n     - 64\n')
        text = text.replace('     - Write only\n',
                            '     - Write only\n     - 64\n')
        cases, _, _, errors = scan(text)
        self.assertEqual(errors, [])
        self.assertIn('0x38', [s['offset'] for s in cases[0]['skipped']])

    def test_window_without_a_reset_is_one_skip(self):
        text = PAGE.replace('``0x38``', '``0x100``-``0x7ff8``').replace(
            'per machine', 'unsupported')
        cases, _, _, errors = scan(text)
        self.assertEqual(errors, [])
        windows = [s for s in cases[0]['skipped'] if s['offset'] == '0x100']
        self.assertEqual(len(windows), 1)
        self.assertIn('slots', windows[0]['reason'])

    def test_setup_steps_come_with_the_table(self):
        marker = MARKER.replace('width=64', 'width=64 setup=0x20:0x1100,'
                                '0x28:0x5:8')
        cases, _, _, errors = scan(PAGE, marker=marker, skip='0x38')
        self.assertEqual(errors, [])
        self.assertEqual(cases[0]['setup'], [
            {'address': '0x20', 'value': '0x1100', 'width': 32},
            {'address': '0x28', 'value': '0x5', 'width': 8}])

    def test_select_writes_ride_with_the_row(self):
        # "Two" shares offsets with other rows in a real map; the writes
        # that choose it belong to that row's reads alone.
        marker = MARKER.replace('width=64',
                                'width=64 select="Two|0x40=0xa0,0x48=1"')
        cases, _, _, errors = scan(PAGE, marker=marker, skip='0x38')
        self.assertEqual(errors, [])
        regs = [r for r in cases[0]['registers'] if r['name'] == 'Two']
        self.assertEqual(len(regs), 2)
        for reg in regs:
            self.assertEqual(reg['select'], [
                {'address': '0x1040', 'width': 64, 'value': '0xa0'},
                {'address': '0x1048', 'width': 64, 'value': '0x1'}])
        other = [r for r in cases[0]['registers'] if r['name'] != 'Two']
        self.assertTrue(all('select' not in r for r in other))

    def test_select_naming_no_row_is_an_error(self):
        marker = MARKER.replace('width=64', 'width=64 select="Nope|0x40=1"')
        _, _, _, errors = scan(PAGE, marker=marker, skip='0x38')
        self.assertEqual(len(errors), 1)
        self.assertIn('select= names no checked row', errors[0])

    def test_select_needs_a_row_and_a_write(self):
        for bad in ('select=Two', 'select="Two|0x40"', 'select="|0x40=1"'):
            marker = MARKER.replace('width=64', 'width=64 ' + bad)
            _, _, _, errors = scan(PAGE, marker=marker, skip='0x38')
            self.assertEqual(len(errors), 1, bad)

    def test_skiprow_leaves_a_row_out_by_name(self):
        marker = MARKER.replace(
            'width=64', 'width=64 skiprow="Two:shared-offset" skip=0x38')
        cases, _, _, errors = scan(PAGE, marker=marker)
        self.assertEqual(errors, [])
        names = {r['name'] for r in cases[0]['registers']}
        self.assertNotIn('Two', names)
        reasons = {s['offset']: s['reason'] for s in cases[0]['skipped']}
        self.assertEqual((reasons['0x8'], reasons['0x10']),
                         ('shared-offset', 'shared-offset'))

    def test_skiprow_naming_no_row_is_an_error(self):
        marker = MARKER.replace('width=64',
                                'width=64 skiprow="Nope:why" skip=0x38')
        _, _, _, errors = scan(PAGE, marker=marker)
        self.assertEqual(len(errors), 1)
        self.assertIn('skiprow= names no row', errors[0])

    def test_count_expands_a_per_unit_offset(self):
        text = PAGE.replace('``0x38``', '``0x40 + 0x10*n``').replace(
            'per machine', '0')
        marker = MARKER.replace('width=64', 'width=64 count=3')
        cases, _, _, errors = scan(text, marker=marker)
        self.assertEqual(errors, [])
        offsets = [r['offset'] for r in cases[0]['registers']
                   if r['name'] == 'Prose']
        self.assertEqual(offsets, ['0x40', '0x50', '0x60'])

    def test_per_unit_offset_without_count_is_an_error(self):
        text = PAGE.replace('``0x38``', '``0x40 + 0x10*n``').replace(
            'per machine', '0')
        _, _, _, errors = scan(text)
        self.assertEqual(len(errors), 1)
        self.assertIn('needs count=', errors[0])

    def test_label_names_the_configuration(self):
        marker = MARKER.replace('width=64', 'width=64 label=second')
        cases, _, _, errors = scan(PAGE, marker=marker, skip='0x38')
        self.assertEqual(errors, [])
        self.assertEqual(cases[0]['label'], 'second')

    def test_a_value_for_each_offset(self):
        text = PAGE.replace('``0x08``, ``0x10``', '``0x08``, ``0x10``'
                            ).replace('     - 0\n', '     - ``0x5``, 0\n', 1)
        cases, _, _, errors = scan(text, skip='0x38')
        self.assertEqual(errors, [])
        regs = {r['offset']: r['expect'] for r in cases[0]['registers']}
        self.assertEqual((regs['0x8'], regs['0x10']), ('0x5', '0x0'))

    def test_values_for_the_wrong_number_of_offsets_is_an_error(self):
        text = PAGE.replace('     - 0\n', '     - 1, 2, 3\n', 1)
        _, _, _, errors = scan(text, skip='0x38')
        self.assertEqual(len(errors), 1)
        self.assertIn('3 reset values for 2 offsets', errors[0])

    def test_skiprow_of_a_wide_window_is_one_entry(self):
        text = PAGE.replace('``0x38``', '``0x100``-``0x7ff8``').replace(
            'per machine', '0')
        marker = MARKER.replace('width=64', 'width=64 skiprow="Prose:bulk"')
        cases, _, _, errors = scan(text, marker=marker)
        self.assertEqual(errors, [])
        windows = [s for s in cases[0]['skipped'] if s['offset'] == '0x100']
        self.assertEqual(len(windows), 1)
        self.assertEqual(windows[0]['reason'], 'bulk (4064 slots)')

    def test_lane_moves_the_read_not_the_offset(self):
        marker = MARKER.replace('width=64', 'width=64 lane=4')
        cases, _, _, errors = scan(PAGE, marker=marker, skip='0x38')
        self.assertEqual(errors, [])
        reg = [r for r in cases[0]['registers'] if r['offset'] == '0x0'][0]
        self.assertEqual(reg['address'], '0x1004')

    def test_stride_spaces_a_range(self):
        marker = MARKER.replace('width=64', 'width=64 stride=16')
        cases, _, _, errors = scan(PAGE, marker=marker, skip='0x38')
        self.assertEqual(errors, [])
        offsets = [r['offset'] for r in cases[0]['registers']
                   if r['name'] == 'Range']
        self.assertEqual(offsets, ['0x20'])

    def test_lane_and_stride_must_be_numbers(self):
        for bad in ('lane=x', 'stride=0', 'stride=y'):
            marker = MARKER.replace('width=64', 'width=64 ' + bad)
            _, _, _, errors = scan(PAGE, marker=marker, skip='0x38')
            self.assertEqual(len(errors), 1, bad)

    def test_units_expand_a_row_relative_to_its_unit(self):
        text = PAGE.replace('``0x38``', 'link ``+0x08``, ``+0x10``'
                            ).replace('per machine', '0')
        marker = MARKER.replace('width=64', 'width=64 units=0x100:0x40:2')
        cases, _, _, errors = scan(text, marker=marker)
        self.assertEqual(errors, [])
        offsets = [r['offset'] for r in cases[0]['registers']
                   if r['name'] == 'Prose']
        self.assertEqual(offsets, ['0x108', '0x110', '0x148', '0x150'])

    def test_unit_row_without_units_is_an_error(self):
        text = PAGE.replace('``0x38``', 'link ``+0x08``').replace(
            'per machine', '0')
        _, _, _, errors = scan(text)
        self.assertEqual(len(errors), 1)
        self.assertIn('needs units=', errors[0])

    def test_this_tree(self):
        cases, annotated, other, errors = tables.scan(SOURCE)
        self.assertEqual(errors, [])
        self.assertGreaterEqual(len(annotated), 3)
        for case in cases:
            self.assertTrue(case['registers'], case['id'])


if __name__ == '__main__':
    unittest.main()
