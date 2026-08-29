# Tests for the matrix manifest parser and per-cell command-line builder
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_lib_matrix.py` (or `uv run --with
pytest pytest tests/functional/mips64/test_lib_matrix.py -v`), the same
way the *_verdict.py companions in this directory run.
"""

import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from lib_matrix import (AttemptResult, Cell, MatrixError, machine_props,
                        matrix_rows, parse_matrix, parse_row,
                        pass_condition_met, peek_row, prom_args, qemu_args,
                        verdict)
from lib_prom import MARK, first_progress_code


STARTER_MANIFEST = Path(__file__).parent / 'origin200-matrix'

# The field order parse_matrix() expects, matching the Cell dataclass.
FIELDS = ('id', 'guest', 'scenario', 'cpus', 'nodes', 'mem', 'accel',
          'firmware', 'speed', 'timeout', 'attempts', 'min_passes',
          'expect', 'xfail_mark', 'xfail_signature')

# A known-good row, as field/value pairs, so each test can override just
# the field it cares about rather than restating all fifteen.
BASE_ROW = {
    'id': 'firmware_smoke',
    'guest': 'prom',
    'scenario': 'smoke',
    'cpus': '1',
    'nodes': '1',
    'mem': '64M',
    'accel': 'wallclock',
    'firmware': 'synth',
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


class _ManifestTest(unittest.TestCase):
    """Base for the tests that write a throwaway manifest."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp_path = Path(tmp.name)

    def write_manifest(self, *lines):
        manifest = self.tmp_path / 'matrix'
        manifest.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        return manifest


class StarterManifestTest(unittest.TestCase):

    def test_parses_the_smoke_cell(self):
        cells = parse_matrix(STARTER_MANIFEST)
        by_id = {cell.id: cell for cell in cells}

        cell = by_id['firmware_smoke']
        self.assertIsInstance(cell, Cell)
        self.assertEqual(cell.cpus, 1)
        self.assertEqual(cell.speed, 'quick')
        self.assertEqual(cell.timeout, 120)
        self.assertEqual(cell.expect, 'pass')

    def test_parses_the_config_cells(self):
        # The three real firmware-boot cells this task adds: 4-CPU single
        # node, two-node, and mttcg -- each expect=pass (never xfail: a
        # config that does not boot must fail red) and firmware=prom (needs
        # the provisioned PROM, so it skips rather than xfails when absent).
        cells = parse_matrix(STARTER_MANIFEST)
        by_id = {cell.id: cell for cell in cells}

        self.assertEqual(len(cells), 4)

        four_cpu = by_id['prom_full_boot_4cpu']
        self.assertEqual(
            (four_cpu.scenario, four_cpu.cpus, four_cpu.nodes,
             four_cpu.accel), ('full_boot', 4, 1, 'wallclock'))

        two_node = by_id['prom_full_boot_2node']
        self.assertEqual(
            (two_node.scenario, two_node.cpus, two_node.nodes,
             two_node.accel), ('full_boot', 4, 2, 'wallclock'))

        mttcg = by_id['prom_full_boot_mttcg']
        self.assertEqual(
            (mttcg.scenario, mttcg.cpus, mttcg.nodes, mttcg.accel),
            ('full_boot', 2, 1, 'mttcg'))

        for cell in (four_cpu, two_node, mttcg):
            self.assertEqual(cell.guest, 'prom')
            self.assertEqual(cell.firmware, 'prom')
            self.assertEqual(cell.speed, 'thorough')
            self.assertEqual(cell.timeout, 700)
            self.assertEqual(cell.expect, 'pass')
            self.assertEqual(cell.xfail_mark, '')
            self.assertEqual(cell.xfail_signature, '')


class ParseMatrixTest(_ManifestTest):

    def test_ignores_comments_and_blanks(self):
        manifest = self.write_manifest(
            '# a comment line',
            '',
            make_row(),
        )

        cells = parse_matrix(manifest)

        self.assertEqual(len(cells), 1)

    def test_rejects_duplicate_id(self):
        manifest = self.write_manifest(
            make_row(id='dup_cell'),
            make_row(id='dup_cell'),
        )

        with self.assertRaisesRegex(MatrixError, 'dup_cell'):
            parse_matrix(manifest)

    def test_rejects_non_identifier_id(self):
        manifest = self.write_manifest(make_row(id='a b'))

        with self.assertRaises(MatrixError):
            parse_matrix(manifest)

    def test_rejects_bad_speed(self):
        manifest = self.write_manifest(make_row(speed='fast'))

        with self.assertRaises(MatrixError):
            parse_matrix(manifest)

    def test_rejects_bad_guest(self):
        manifest = self.write_manifest(make_row(guest='solaris'))

        with self.assertRaises(MatrixError):
            parse_matrix(manifest)

    def test_rejects_bad_accel(self):
        manifest = self.write_manifest(make_row(accel='kvm'))

        with self.assertRaises(MatrixError):
            parse_matrix(manifest)

    def test_rejects_bad_expect(self):
        manifest = self.write_manifest(make_row(expect='maybe'))

        with self.assertRaises(MatrixError):
            parse_matrix(manifest)

    def test_rejects_bad_firmware(self):
        manifest = self.write_manifest(make_row(firmware='prm'))

        with self.assertRaises(MatrixError):
            parse_matrix(manifest)

    def test_rejects_xfail_without_signature(self):
        manifest = self.write_manifest(
            make_row(expect='xfail', xfail_mark='firmware',
                     xfail_signature=''),
        )

        with self.assertRaises(MatrixError):
            parse_matrix(manifest)

    def test_rejects_wrong_field_count(self):
        manifest = self.write_manifest('firmware_smoke | prom | smoke')

        with self.assertRaises(MatrixError):
            parse_matrix(manifest)


class RowApiTest(_ManifestTest):
    """The row-at-a-time API the matrix test builds its methods from."""

    def test_matrix_rows_numbers_data_rows_and_skips_the_rest(self):
        manifest = self.write_manifest(
            '# a comment line',
            '',
            make_row(id='first'),
            '   ',
            make_row(id='second'),
        )

        rows = matrix_rows(manifest)

        self.assertEqual([lineno for lineno, _line in rows], [3, 5])
        # Rows come back stripped, the same text parse_row() sees.
        self.assertEqual(rows[0][1], make_row(id='first').strip())

    def test_matrix_rows_does_not_parse(self):
        # A row parse_matrix() would reject is still enumerated: judging
        # it is parse_row()'s job, one test at a time.
        manifest = self.write_manifest('not | a | row')

        self.assertEqual(matrix_rows(manifest), [(1, 'not | a | row')])

    def test_parse_row_parses_one_row(self):
        cell = parse_row('matrix', 7,
                         make_row(id='one_cell', speed='thorough'))

        self.assertEqual(cell.id, 'one_cell')
        self.assertEqual(cell.speed, 'thorough')
        self.assertEqual(cell.cpus, 1)

    def test_parse_row_names_the_line_it_rejects(self):
        with self.assertRaisesRegex(MatrixError, 'matrix:7'):
            parse_row('matrix', 7, make_row(speed='fast'))

    def test_peek_row_reads_id_and_speed_raw(self):
        self.assertEqual(peek_row(make_row(id='a_cell', speed='thorough')),
                         ('a_cell', 'thorough'))

    def test_peek_row_blanks_what_it_cannot_use(self):
        # Wrong field count: neither value is usable.
        self.assertEqual(peek_row('firmware_smoke | prom | smoke'),
                         (None, None))
        # A malformed id or an unknown speed is blanked on its own, the
        # other field still read.
        self.assertEqual(peek_row(make_row(id='a b')), (None, 'quick'))
        self.assertEqual(peek_row(make_row(speed='fast')),
                         ('firmware_smoke', None))

    def test_peek_row_validates_nothing_else(self):
        # A row parse_row() rejects (bad guest) still peeks: the peek
        # exists to name and route the test that will then fail on
        # parse_row().
        row = make_row(guest='solaris')

        self.assertEqual(peek_row(row), ('firmware_smoke', 'quick'))
        with self.assertRaises(MatrixError):
            parse_row('matrix', 1, row)


# A known-good Cell, typed rather than string-valued, so each test can
# override just the field it cares about.
BASE_CELL = Cell(
    id='firmware_smoke',
    guest='prom',
    scenario='smoke',
    cpus=1,
    nodes=1,
    mem='64M',
    accel='wallclock',
    firmware='synth',
    speed='quick',
    timeout=120,
    attempts=1,
    min_passes=1,
    expect='pass',
    xfail_mark='',
    xfail_signature='',
)


def make_cell(**overrides):
    return replace(BASE_CELL, **overrides)


class QemuArgsTest(unittest.TestCase):
    """The per-cell QEMU command-line builder."""

    def test_machine_props_prom(self):
        synth_cell = make_cell(firmware='synth')
        self.assertEqual(machine_props(synth_cell),
                         'origin200,synthesized-firmware=on,debug-leds=on')

        prom_cell = make_cell(firmware='prom')
        self.assertEqual(machine_props(prom_cell),
                         'origin200,synthesized-firmware=off,debug-leds=on')

    def test_icount_regime(self):
        args = qemu_args(make_cell(accel='icount'))
        joined = ' '.join(args)

        self.assertIn('-accel tcg,thread=single', joined)
        self.assertIn('-icount shift=2,align=off,sleep=on', joined)

    def test_mttcg_regime(self):
        args = qemu_args(make_cell(accel='mttcg'))
        joined = ' '.join(args)

        self.assertIn('-accel tcg,thread=multi', joined)
        self.assertNotIn('-icount', args)

    def test_wallclock_regime(self):
        args = qemu_args(make_cell(accel='wallclock'))
        joined = ' '.join(args)

        self.assertIn('-accel tcg,thread=single', joined)
        self.assertNotIn('-icount', args)

    def test_single_node_no_numa(self):
        args = qemu_args(make_cell(cpus=1, nodes=1))
        joined = ' '.join(args)

        self.assertIn('-smp 1', joined)
        self.assertNotIn('-numa', args)

    def test_two_node_numa(self):
        args = qemu_args(make_cell(cpus=4, nodes=2, mem='128M'))
        joined = ' '.join(args)

        self.assertIn('-smp 4,sockets=2,cores=2,threads=1', joined)
        self.assertIn('-object memory-backend-ram,id=ram0,size=64M', joined)
        self.assertIn('-object memory-backend-ram,id=ram1,size=64M', joined)
        self.assertIn('-numa node,nodeid=0,memdev=ram0,cpus=0-1', joined)
        self.assertIn('-numa node,nodeid=1,memdev=ram1,cpus=2-3', joined)

    def test_cpus_not_divisible_by_nodes_rejected(self):
        with self.assertRaises(MatrixError):
            qemu_args(make_cell(cpus=3, nodes=2, mem='128M'))


class PromArgsTest(unittest.TestCase):
    """The real-PROM QEMU args (lib_prom's resolve_asset finds the
    image; test_lib_prom.py covers it)."""

    def test_prom_args_with_resolved_firmware(self):
        self.assertEqual(
            prom_args('/some/checkout/assets/firmware/6.5-base/'
                      'ip27prom.payload.bin'),
            ['-bios', '/some/checkout/assets/firmware/6.5-base/'
                      'ip27prom.payload.bin',
             '-d', 'trace:sgi_hub_led'])

    def test_prom_args_without_firmware(self):
        # A firmware=synth cell never resolves an image, so the caller
        # passes None straight through -- must contribute no args at all.
        self.assertEqual(prom_args(None), [])


# Cells for the pass-condition/retry/expected-failure verdict tests

PASS_CELL = make_cell(expect='pass', attempts=3, min_passes=2)

XFAIL_CELL = make_cell(expect='xfail', attempts=1, min_passes=1,
                       xfail_mark='firmware',
                       xfail_signature='reset_vector_trap')


class VerdictTest(unittest.TestCase):

    def test_pass_meets_threshold(self):
        attempts = [
            AttemptResult(reached_pass=True),
            AttemptResult(reached_pass=True),
            AttemptResult(reached_pass=False),
        ]

        self.assertEqual(verdict(PASS_CELL, attempts), 'pass')

    def test_fail_below_threshold(self):
        attempts = [
            AttemptResult(reached_pass=True),
            AttemptResult(reached_pass=False),
            AttemptResult(reached_pass=False),
        ]

        self.assertEqual(verdict(PASS_CELL, attempts), 'fail')

    def test_infra_error_is_error_not_pass(self):
        attempts = [
            AttemptResult(reached_pass=True),
            AttemptResult(reached_pass=False,
                          infra_error='missing asset: origin200-prom.bin'),
            AttemptResult(reached_pass=True),
        ]

        self.assertEqual(verdict(PASS_CELL, attempts), 'error')

    def test_xfail_matching_signature(self):
        attempts = [
            AttemptResult(reached_pass=False,
                          signature='reset_vector_trap'),
        ]

        self.assertEqual(verdict(XFAIL_CELL, attempts), 'xfail')

    def test_xfail_wrong_signature_is_fail(self):
        attempts = [
            AttemptResult(reached_pass=False,
                          signature='unexpected_nmi'),
        ]

        self.assertEqual(verdict(XFAIL_CELL, attempts), 'fail')

    def test_xfail_that_passes_is_xpass(self):
        attempts = [AttemptResult(reached_pass=True)]

        self.assertEqual(verdict(XFAIL_CELL, attempts), 'xpass')

    def test_infra_error_empty_string_still_counts_as_set(self):
        # infra_error is SET (not None), even though '' is falsy — must
        # not be skipped by a truthiness check.
        attempts = [AttemptResult(reached_pass=True, infra_error='')]

        self.assertEqual(verdict(PASS_CELL, attempts), 'error')

    def test_infra_error_on_xfail_cell_is_error(self):
        attempts = [AttemptResult(reached_pass=False,
                                  infra_error='qemu crashed')]

        self.assertEqual(verdict(XFAIL_CELL, attempts), 'error')

    def test_verdict_raises_on_no_attempts(self):
        with self.assertRaises(MatrixError):
            verdict(PASS_CELL, [])

    def test_xfail_multi_attempt_mismatched_signatures_is_fail(self):
        # Two attempts both miss their pass condition, but with different
        # failure signatures — only one of them matches xfail_signature,
        # so the cell did not fail the one specific way it's allowed to.
        attempts = [
            AttemptResult(reached_pass=False, signature='reset_vector_trap'),
            AttemptResult(reached_pass=False, signature='unexpected_nmi'),
        ]

        self.assertEqual(verdict(XFAIL_CELL, attempts), 'fail')

    def test_xfail_multi_attempt_matching_signatures_is_xfail(self):
        # Two attempts both miss their pass condition with the same
        # expected signature — every failing attempt agrees, so the cell
        # is xfail.
        attempts = [
            AttemptResult(reached_pass=False, signature='reset_vector_trap'),
            AttemptResult(reached_pass=False, signature='reset_vector_trap'),
        ]

        self.assertEqual(verdict(XFAIL_CELL, attempts), 'xfail')


class FirstProgressCodeTest(unittest.TestCase):

    def test_first_progress_code_found(self):
        text = ('some preceding boot noise\n'
                'sgi_hub_led CPU 0: 0x03 (progress: something)\n'
                'trailing noise\n')

        self.assertEqual(first_progress_code(text), '0x03')

    def test_first_progress_code_absent(self):
        text = 'no progress codes in this console capture at all\n'

        self.assertIsNone(first_progress_code(text))

    def test_first_progress_code_ignores_failure_kind(self):
        # The trace event uses the identical line shape for a diagnosed
        # self-test failure (ECC, FPU, ...); scoring that as progress
        # would be a false pass for a real firmware cell that hit one.
        text = 'sgi_hub_led CPU 0: 0x81 (FAILURE: something)\n'

        self.assertIsNone(first_progress_code(text))

    def test_first_progress_code_skips_failure_to_find_progress(self):
        text = ('sgi_hub_led CPU 0: 0x81 (FAILURE: something)\n'
                'sgi_hub_led CPU 0: 0x03 (progress: something)\n')

        self.assertEqual(first_progress_code(text), '0x03')


def _trace_line(code, kind='progress', cpu=0, meaning='test fixture'):
    """One sgi_hub_led trace line, in the exact shape QEMU emits it --
    matches test_prom_boot_verdict.py's own fixture helper."""
    return f'sgi_hub_led CPU {cpu}: {code} ({kind}: {meaning})'


class PassConditionTest(unittest.TestCase):
    """The firmware-boot pass condition (the progress mark)."""

    def test_pass_condition_met_full_mark_passes(self):
        text = '\n'.join(_trace_line(c) for c in sorted(MARK))

        self.assertIs(pass_condition_met('full_boot', text), True)

    def test_pass_condition_met_partial_mark_fails(self):
        # Some codes seen, not the whole mark -- a wedge that stops after
        # an early code must not satisfy a real firmware-boot cell's pass
        # condition. This is the strengthening this task adds: the old
        # first-progress-code check would have scored this a pass.
        partial = sorted(MARK)[:len(MARK) // 2]
        text = '\n'.join(_trace_line(c) for c in partial)

        self.assertIs(pass_condition_met('full_boot', text), False)

    def test_pass_condition_met_empty_log_fails(self):
        self.assertIs(pass_condition_met('full_boot', ''), False)

    def test_pass_condition_met_extra_codes_beyond_mark_still_pass(self):
        # Subset, not equality -- a boot that goes further than the
        # recorded mark is not a regression. Mirrors lib_prom's own
        # mark_reached() contract.
        text = '\n'.join(_trace_line(c) for c in sorted(MARK)) + '\n' + \
            _trace_line('0xfe')

        self.assertIs(pass_condition_met('full_boot', text), True)

    def test_pass_condition_met_smoke_passes_on_first_code(self):
        # The smoke scenario keeps its original, weaker behaviour: any
        # single progress code is enough, not the whole mark.
        text = _trace_line(sorted(MARK)[0])

        self.assertIs(pass_condition_met('smoke', text), True)

    def test_pass_condition_met_smoke_fails_with_no_code(self):
        self.assertIs(pass_condition_met('smoke', ''), False)

    def test_pass_condition_met_smoke_ignores_failure_kind(self):
        # Matches first_progress_code()'s own kind filter: a FAILURE line
        # must not score as a smoke pass.
        text = _trace_line('0x81', kind='FAILURE')

        self.assertIs(pass_condition_met('smoke', text), False)


if __name__ == '__main__':
    unittest.main()
