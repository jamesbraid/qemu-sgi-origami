# Tests for the graphics matrix manifest parser and scenario registry
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_lib_graphics_matrix.py`, the same
way test_lib_matrix.py runs.
"""

import unittest
from pathlib import Path

from lib_graphics_matrix import (SCENARIOS, GraphicsMatrixError, disk_rel,
                                 parse_row, peek_row)


STARTER_MANIFEST = Path(__file__).parent / 'graphics-matrix'

FIELDS = ('id', 'guest', 'device', 'disk', 'scenario', 'cpus', 'mem',
          'accel', 'speed', 'timeout', 'attempts', 'min_passes',
          'expect', 'xfail_mark', 'xfail_signature')

BASE_ROW = {
    'id': 'sample_cell',
    'guest': 'irix',
    'device': 'psitech-rad4,addr=5',
    'disk': 'root-rad4',
    'scenario': 'rad4_login_screen',
    'cpus': '1',
    'mem': '64M',
    'accel': 'wallclock',
    'speed': 'quick',
    'timeout': '120',
    'attempts': '1',
    'min_passes': '1',
    'expect': 'pass',
    'xfail_mark': '',
    'xfail_signature': '',
}


def make_row(**overrides):
    values = dict(BASE_ROW, **overrides)
    return ' | '.join(values[field] for field in FIELDS)


class StarterManifestTest(unittest.TestCase):

    def test_parses_the_rad4_cell(self):
        with open(STARTER_MANIFEST, encoding='utf-8') as fobj:
            lines = [line.strip() for line in fobj
                     if line.strip() and not line.strip().startswith('#')]

        self.assertEqual(len(lines), 1)
        cell = parse_row(STARTER_MANIFEST, 2, lines[0])

        self.assertEqual(cell.id, 'rad4_login_screen')
        self.assertEqual(cell.guest, 'irix')
        self.assertEqual(cell.device, 'psitech-rad4,addr=5')
        self.assertEqual(cell.disk, 'root-rad4')
        self.assertEqual(cell.scenario, 'rad4_login_screen')
        self.assertEqual(cell.expect, 'pass')
        self.assertEqual(cell.nodes, 1)


class ParseRowTest(unittest.TestCase):

    def test_parses_a_good_row(self):
        cell = parse_row('matrix', 5, make_row())

        self.assertEqual(cell.id, 'sample_cell')
        self.assertEqual(cell.cpus, 1)
        self.assertEqual(cell.nodes, 1)

    def test_rejects_wrong_field_count(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, 'a | b | c')

    def test_rejects_bad_guest(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, make_row(guest='linux'))

    def test_rejects_empty_device(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, make_row(device=''))

    def test_rejects_empty_disk(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, make_row(disk=''))

    def test_rejects_unknown_scenario(self):
        with self.assertRaisesRegex(GraphicsMatrixError, 'SCENARIOS'):
            parse_row('matrix', 1, make_row(scenario='no_such_scenario'))

    def test_rejects_bad_speed(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, make_row(speed='fast'))

    def test_rejects_bad_accel(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, make_row(accel='kvm'))

    def test_rejects_xfail_without_signature(self):
        with self.assertRaises(GraphicsMatrixError):
            parse_row('matrix', 1, make_row(expect='xfail'))

    def test_accepts_xfail_with_signature(self):
        cell = parse_row('matrix', 1, make_row(
            expect='xfail', xfail_mark='screen',
            xfail_signature='black-screen'))

        self.assertEqual(cell.expect, 'xfail')


class PeekRowTest(unittest.TestCase):

    def test_reads_id_and_speed(self):
        self.assertEqual(
            peek_row(make_row(id='a_cell', speed='thorough')),
            ('a_cell', 'thorough'))

    def test_blanks_what_it_cannot_use(self):
        self.assertEqual(peek_row('a | b | c'), (None, None))


class ScenarioRegistryTest(unittest.TestCase):

    def test_rad4_login_screen_is_registered(self):
        self.assertIn('rad4_login_screen', SCENARIOS)

    def test_rad4_login_screen_has_a_not_all_black_check(self):
        scenario = SCENARIOS['rad4_login_screen']
        kinds = [type(check).__name__ for check in scenario.screen_checks]

        self.assertIn('NotAllBlackCheck', kinds)
        self.assertIn('RegionColorCheck', kinds)


class DiskRelTest(unittest.TestCase):

    def test_builds_the_sibling_asset_convention_path(self):
        # Matches test_irix_diag.py's own DISK_REL by hand
        # ('assets/irix/6.5.30/root-diags.qcow2') for a different variant
        # name, so a new variant needs no new constant, just a new
        # manifest row naming it.
        self.assertEqual(disk_rel('root-rad4'),
                         'assets/irix/6.5.30/root-rad4.qcow2')


if __name__ == '__main__':
    unittest.main()
