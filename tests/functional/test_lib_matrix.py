# Tests for the matrix manifest parser and per-cell command-line builder
#
# SPDX-License-Identifier: GPL-2.0-or-later

from dataclasses import replace
from pathlib import Path

import pytest

from lib_matrix import (Cell, MatrixError, machine_props, parse_matrix,
                         qemu_args)


STARTER_MANIFEST = Path(__file__).parent / 'mips64' / 'origin200-matrix'

# The field order parse_matrix() expects, matching the Cell dataclass.
FIELDS = ('id', 'guest', 'scenario', 'cpus', 'nodes', 'mem', 'accel',
          'firmware', 'speed', 'timeout', 'attempts', 'min_passes',
          'expect', 'xfail_oracle', 'xfail_signature')

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
    'xfail_oracle': '',
    'xfail_signature': '',
}


def make_row(**overrides):
    values = dict(BASE_ROW, **overrides)
    return ' | '.join(values[field] for field in FIELDS)


def write_manifest(tmp_path, *lines):
    manifest = tmp_path / 'matrix'
    manifest.write_text('\n'.join(lines) + '\n')
    return manifest


def test_parses_the_smoke_cell():
    cells = parse_matrix(STARTER_MANIFEST)

    assert len(cells) == 1
    cell = cells[0]
    assert isinstance(cell, Cell)
    assert cell.id == 'firmware_smoke'
    assert cell.cpus == 1
    assert cell.speed == 'quick'
    assert cell.timeout == 120
    assert cell.expect == 'pass'


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


def test_rejects_xfail_without_signature(tmp_path):
    manifest = write_manifest(
        tmp_path,
        make_row(expect='xfail', xfail_oracle='firmware',
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
    xfail_oracle='',
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
