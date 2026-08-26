# Tests for the matrix manifest parser and per-cell command-line builder
#
# SPDX-License-Identifier: GPL-2.0-or-later

import sys
from dataclasses import replace
from pathlib import Path

import pytest

from lib_matrix import (AttemptResult, Cell, FIRMWARE_REL_PATH, MatrixError,
                         first_progress_code, machine_props, parse_matrix,
                         pass_condition_met, prom_args, qemu_args,
                         resolve_prom, verdict)

# test_prom_boot.py lives one directory down (tests/functional/mips64/), not
# on sys.path by default here -- inserted so test_firmware_rel_path_matches_
# test_prom_boot below can import its FIRMWARE_REL directly, the same way
# test_prom_boot.py itself inserts tests/functional to reach lib_matrix.
# Also brings in MARK, for the pass-condition tests further down: it and
# FIRMWARE_REL both sit outside test_prom_boot's qemu_test-guarded block, so
# this import needs no build.
sys.path.insert(0, str(Path(__file__).parent / 'mips64'))
from test_prom_boot import FIRMWARE_REL as PROM_BOOT_FIRMWARE_REL  # noqa: E402
from test_prom_boot import MARK  # noqa: E402


STARTER_MANIFEST = Path(__file__).parent / 'mips64' / 'origin200-matrix'

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


def write_manifest(tmp_path, *lines):
    manifest = tmp_path / 'matrix'
    manifest.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return manifest


def test_parses_the_smoke_cell():
    cells = parse_matrix(STARTER_MANIFEST)
    by_id = {cell.id: cell for cell in cells}

    cell = by_id['firmware_smoke']
    assert isinstance(cell, Cell)
    assert cell.cpus == 1
    assert cell.speed == 'quick'
    assert cell.timeout == 120
    assert cell.expect == 'pass'


def test_parses_the_config_cells():
    # The three real firmware-boot cells this task adds: 4-CPU single
    # node, two-node, and mttcg -- each expect=pass (never xfail: a
    # config that does not boot must fail red) and firmware=prom (needs
    # the provisioned PROM, so it skips rather than xfails when absent).
    cells = parse_matrix(STARTER_MANIFEST)
    by_id = {cell.id: cell for cell in cells}

    assert len(cells) == 4

    four_cpu = by_id['prom_full_boot_4cpu']
    assert (four_cpu.scenario, four_cpu.cpus, four_cpu.nodes,
            four_cpu.accel) == ('full_boot', 4, 1, 'wallclock')

    two_node = by_id['prom_full_boot_2node']
    assert (two_node.scenario, two_node.cpus, two_node.nodes,
            two_node.accel) == ('full_boot', 4, 2, 'wallclock')

    mttcg = by_id['prom_full_boot_mttcg']
    assert (mttcg.scenario, mttcg.cpus, mttcg.nodes,
            mttcg.accel) == ('full_boot', 2, 1, 'mttcg')

    for cell in (four_cpu, two_node, mttcg):
        assert cell.guest == 'prom'
        assert cell.firmware == 'prom'
        assert cell.speed == 'thorough'
        assert cell.timeout == 700
        assert cell.expect == 'pass'
        assert cell.xfail_mark == ''
        assert cell.xfail_signature == ''


def test_ignores_comments_and_blanks(tmp_path):
    manifest = write_manifest(
        tmp_path,
        '# a comment line',
        '',
        make_row(),
    )

    cells = parse_matrix(manifest)

    assert len(cells) == 1


def test_rejects_duplicate_id(tmp_path):
    manifest = write_manifest(
        tmp_path,
        make_row(id='dup_cell'),
        make_row(id='dup_cell'),
    )

    with pytest.raises(MatrixError, match='dup_cell'):
        parse_matrix(manifest)


def test_rejects_non_identifier_id(tmp_path):
    manifest = write_manifest(tmp_path, make_row(id='a b'))

    with pytest.raises(MatrixError):
        parse_matrix(manifest)


def test_rejects_bad_speed(tmp_path):
    manifest = write_manifest(tmp_path, make_row(speed='fast'))

    with pytest.raises(MatrixError):
        parse_matrix(manifest)


def test_rejects_bad_guest(tmp_path):
    manifest = write_manifest(tmp_path, make_row(guest='solaris'))

    with pytest.raises(MatrixError):
        parse_matrix(manifest)


def test_rejects_bad_accel(tmp_path):
    manifest = write_manifest(tmp_path, make_row(accel='kvm'))

    with pytest.raises(MatrixError):
        parse_matrix(manifest)


def test_rejects_bad_expect(tmp_path):
    manifest = write_manifest(tmp_path, make_row(expect='maybe'))

    with pytest.raises(MatrixError):
        parse_matrix(manifest)


def test_rejects_bad_firmware(tmp_path):
    manifest = write_manifest(tmp_path, make_row(firmware='prm'))

    with pytest.raises(MatrixError):
        parse_matrix(manifest)


def test_rejects_xfail_without_signature(tmp_path):
    manifest = write_manifest(
        tmp_path,
        make_row(expect='xfail', xfail_mark='firmware',
                 xfail_signature=''),
    )

    with pytest.raises(MatrixError):
        parse_matrix(manifest)


def test_rejects_wrong_field_count(tmp_path):
    manifest = write_manifest(tmp_path, 'firmware_smoke | prom | smoke')

    with pytest.raises(MatrixError):
        parse_matrix(manifest)


# Tests for the per-cell QEMU command-line builder

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


def test_machine_props_prom():
    synth_cell = make_cell(firmware='synth')
    assert (machine_props(synth_cell) ==
            'origin200,synthesized-firmware=on,debug-leds=on')

    prom_cell = make_cell(firmware='prom')
    assert (machine_props(prom_cell) ==
            'origin200,synthesized-firmware=off,debug-leds=on')


def test_icount_regime():
    args = qemu_args(make_cell(accel='icount'))
    joined = ' '.join(args)

    assert '-accel tcg,thread=single' in joined
    assert '-icount shift=2,align=off,sleep=on' in joined


def test_mttcg_regime():
    args = qemu_args(make_cell(accel='mttcg'))
    joined = ' '.join(args)

    assert '-accel tcg,thread=multi' in joined
    assert '-icount' not in args


def test_wallclock_regime():
    args = qemu_args(make_cell(accel='wallclock'))
    joined = ' '.join(args)

    assert '-accel tcg,thread=single' in joined
    assert '-icount' not in args


def test_single_node_no_numa():
    args = qemu_args(make_cell(cpus=1, nodes=1))
    joined = ' '.join(args)

    assert '-smp 1' in joined
    assert '-numa' not in args


def test_two_node_numa():
    args = qemu_args(make_cell(cpus=4, nodes=2, mem='128M'))
    joined = ' '.join(args)

    assert '-smp 4,sockets=2,cores=2,threads=1' in joined
    assert '-object memory-backend-ram,id=ram0,size=64M' in joined
    assert '-object memory-backend-ram,id=ram1,size=64M' in joined
    assert '-numa node,nodeid=0,memdev=ram0,cpus=0-1' in joined
    assert '-numa node,nodeid=1,memdev=ram1,cpus=2-3' in joined


def test_cpus_not_divisible_by_nodes_rejected():
    with pytest.raises(MatrixError):
        qemu_args(make_cell(cpus=3, nodes=2, mem='128M'))


# Tests for the real-PROM path resolver and its QEMU args

def test_resolve_prom_present(tmp_path):
    firmware_dir = tmp_path / 'assets' / 'firmware' / '6.5-base'
    firmware_dir.mkdir(parents=True)
    firmware_path = firmware_dir / 'ip27prom.payload.bin'
    firmware_path.write_bytes(b'')

    assert resolve_prom(str(tmp_path)) == str(firmware_path)


def test_resolve_prom_absent(tmp_path):
    # tmp_path has no assets/firmware/6.5-base/ip27prom.payload.bin at
    # all -- the runner may not have it provisioned yet.
    assert resolve_prom(str(tmp_path)) is None


def test_resolve_prom_no_assets_root():
    assert resolve_prom(None) is None
    assert resolve_prom('') is None


def test_firmware_rel_path_matches_test_prom_boot():
    # A real cross-file check against test_prom_boot.py's own FIRMWARE_REL
    # (imported above), not a second hardcoded copy of the literal -- a
    # copy would pass even if one of the two files' paths drifted from
    # the other, which is exactly the drift this test exists to catch.
    assert FIRMWARE_REL_PATH == PROM_BOOT_FIRMWARE_REL


def test_prom_args_with_resolved_firmware():
    assert prom_args('/some/checkout/assets/firmware/6.5-base/'
                     'ip27prom.payload.bin') == [
        '-bios', '/some/checkout/assets/firmware/6.5-base/'
                 'ip27prom.payload.bin',
        '-d', 'trace:sgi_hub_led',
    ]


def test_prom_args_without_firmware():
    # A firmware=synth cell never resolves an image, so the caller
    # passes None straight through -- must contribute no args at all.
    assert prom_args(None) == []


# Tests for the pass-condition/retry/expected-failure verdict

PASS_CELL = make_cell(expect='pass', attempts=3, min_passes=2)

XFAIL_CELL = make_cell(expect='xfail', attempts=1, min_passes=1,
                        xfail_mark='firmware',
                        xfail_signature='reset_vector_trap')


def test_pass_meets_threshold():
    attempts = [
        AttemptResult(reached_pass=True),
        AttemptResult(reached_pass=True),
        AttemptResult(reached_pass=False),
    ]

    assert verdict(PASS_CELL, attempts) == 'pass'


def test_fail_below_threshold():
    attempts = [
        AttemptResult(reached_pass=True),
        AttemptResult(reached_pass=False),
        AttemptResult(reached_pass=False),
    ]

    assert verdict(PASS_CELL, attempts) == 'fail'


def test_infra_error_is_error_not_pass():
    attempts = [
        AttemptResult(reached_pass=True),
        AttemptResult(reached_pass=False,
                       infra_error='missing asset: origin200-prom.bin'),
        AttemptResult(reached_pass=True),
    ]

    assert verdict(PASS_CELL, attempts) == 'error'


def test_xfail_matching_signature():
    attempts = [
        AttemptResult(reached_pass=False,
                       signature='reset_vector_trap'),
    ]

    assert verdict(XFAIL_CELL, attempts) == 'xfail'


def test_xfail_wrong_signature_is_fail():
    attempts = [
        AttemptResult(reached_pass=False,
                       signature='unexpected_nmi'),
    ]

    assert verdict(XFAIL_CELL, attempts) == 'fail'


def test_xfail_that_passes_is_xpass():
    attempts = [AttemptResult(reached_pass=True)]

    assert verdict(XFAIL_CELL, attempts) == 'xpass'


def test_infra_error_empty_string_still_counts_as_set():
    # infra_error is SET (not None), even though '' is falsy — must
    # not be skipped by a truthiness check.
    attempts = [AttemptResult(reached_pass=True, infra_error='')]

    assert verdict(PASS_CELL, attempts) == 'error'


def test_infra_error_on_xfail_cell_is_error():
    attempts = [AttemptResult(reached_pass=False,
                               infra_error='qemu crashed')]

    assert verdict(XFAIL_CELL, attempts) == 'error'


def test_verdict_raises_on_no_attempts():
    with pytest.raises(MatrixError):
        verdict(PASS_CELL, [])


def test_xfail_multi_attempt_mismatched_signatures_is_fail():
    # Two attempts both miss their pass condition, but with different
    # failure signatures — only one of them matches xfail_signature, so
    # the cell did not fail the one specific way it's allowed to.
    attempts = [
        AttemptResult(reached_pass=False, signature='reset_vector_trap'),
        AttemptResult(reached_pass=False, signature='unexpected_nmi'),
    ]

    assert verdict(XFAIL_CELL, attempts) == 'fail'


def test_xfail_multi_attempt_matching_signatures_is_xfail():
    # Two attempts both miss their pass condition with the same expected
    # signature — every failing attempt agrees, so the cell is xfail.
    attempts = [
        AttemptResult(reached_pass=False, signature='reset_vector_trap'),
        AttemptResult(reached_pass=False, signature='reset_vector_trap'),
    ]

    assert verdict(XFAIL_CELL, attempts) == 'xfail'


# Tests for the firmware progress-code pass conditions

def test_first_progress_code_found():
    text = ('some preceding boot noise\n'
            'sgi_hub_led CPU 0: 0x03 (progress: something)\n'
            'trailing noise\n')

    assert first_progress_code(text) == '0x03'


def test_first_progress_code_absent():
    text = 'no progress codes in this console capture at all\n'

    assert first_progress_code(text) is None


def test_first_progress_code_ignores_failure_kind():
    # The trace event uses the identical line shape for a diagnosed
    # self-test failure (ECC, FPU, ...); scoring that as progress would
    # be a false pass for a real firmware cell that hit one.
    text = 'sgi_hub_led CPU 0: 0x81 (FAILURE: something)\n'

    assert first_progress_code(text) is None


def test_first_progress_code_skips_failure_to_find_progress():
    text = ('sgi_hub_led CPU 0: 0x81 (FAILURE: something)\n'
            'sgi_hub_led CPU 0: 0x03 (progress: something)\n')

    assert first_progress_code(text) == '0x03'


# Tests for the firmware-boot pass condition (the progress mark)

def _trace_line(code, kind='progress', cpu=0, meaning='test fixture'):
    """One sgi_hub_led trace line, in the exact shape QEMU emits it --
    matches test_prom_boot_verdict.py's own fixture helper."""
    return f'sgi_hub_led CPU {cpu}: {code} ({kind}: {meaning})'


def test_pass_condition_met_full_mark_passes():
    text = '\n'.join(_trace_line(c) for c in sorted(MARK))

    assert pass_condition_met('full_boot', text) is True


def test_pass_condition_met_partial_mark_fails():
    # Some codes seen, not the whole mark -- a wedge that stops after an
    # early code must not satisfy a real firmware-boot cell's pass
    # condition. This is the strengthening this task adds: the old
    # first-progress-code check would have scored this a pass.
    partial = sorted(MARK)[:len(MARK) // 2]
    text = '\n'.join(_trace_line(c) for c in partial)

    assert pass_condition_met('full_boot', text) is False


def test_pass_condition_met_empty_log_fails():
    assert pass_condition_met('full_boot', '') is False


def test_pass_condition_met_extra_codes_beyond_mark_still_pass():
    # Subset, not equality -- a boot that goes further than the recorded
    # mark is not a regression. Mirrors test_prom_boot's own
    # mark_reached() contract.
    text = '\n'.join(_trace_line(c) for c in sorted(MARK)) + '\n' + \
        _trace_line('0xfe')

    assert pass_condition_met('full_boot', text) is True


def test_pass_condition_met_smoke_passes_on_first_code():
    # The smoke scenario keeps its original, weaker behaviour: any single
    # progress code is enough, not the whole mark.
    text = _trace_line(sorted(MARK)[0])

    assert pass_condition_met('smoke', text) is True


def test_pass_condition_met_smoke_fails_with_no_code():
    assert pass_condition_met('smoke', '') is False


def test_pass_condition_met_smoke_ignores_failure_kind():
    # Matches first_progress_code()'s own kind filter: a FAILURE line
    # must not score as a smoke pass.
    text = _trace_line('0x81', kind='FAILURE')

    assert pass_condition_met('smoke', text) is False
