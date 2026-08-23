# The Origin 200 test matrix: manifest format and parser
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The matrix manifest is a pipe-delimited table, one cell per line: `#`
comments and blank lines are ignored, and each row's pipe-separated
fields follow the exact order of the Cell dataclass below.
parse_matrix() is the only entry point here; later stages of this
subsystem (the per-cell command-line builder, the oracle/retry/verdict
logic, the generic test body, and the meson projection) all build on
the Cell records it returns.
"""

from dataclasses import dataclass, fields
from pathlib import Path
import re


class MatrixError(Exception):
    """Raised for any manifest row that fails to parse or validate."""


@dataclass
class Cell:
    id: str
    guest: str
    scenario: str
    cpus: int
    nodes: int
    mem: str
    accel: str
    firmware: str
    speed: str
    timeout: int
    attempts: int
    min_passes: int
    expect: str
    xfail_oracle: str
    xfail_signature: str


# The manifest's field order, taken from Cell itself so the two can
# never drift apart.
_FIELD_NAMES = [f.name for f in fields(Cell)]
_INT_FIELDS = {'cpus', 'nodes', 'timeout', 'attempts', 'min_passes'}

_ID_RE = re.compile(r'^[a-z0-9_]+$')
_VALID_GUESTS = {'prom', 'linux', 'openbsd', 'irix'}
_VALID_SPEEDS = {'quick', 'thorough'}
_VALID_ACCELS = {'icount', 'mttcg', 'wallclock', 'roundrobin'}
_VALID_EXPECTS = {'pass', 'xfail'}


def parse_matrix(path):
    """Parse a pipe-delimited matrix manifest into a list of Cell rows.

    Raises MatrixError on any row that fails to split into the
    expected number of fields, or fails a value check.
    """
    cells = []
    seen_ids = set()
    lines = Path(path).read_text(encoding='utf-8').splitlines()
    for lineno, raw_line in enumerate(lines, start=1):
        line = raw_line.strip()
        if not line or line.startswith('#'):
            continue
        cell = _parse_row(path, lineno, line)
        if cell.id in seen_ids:
            raise MatrixError(
                f'{path}:{lineno}: duplicate cell id {cell.id!r}')
        seen_ids.add(cell.id)
        cells.append(cell)
    return cells


def _parse_row(path, lineno, line):
    raw_fields = [field.strip() for field in line.split('|')]
    if len(raw_fields) != len(_FIELD_NAMES):
        raise MatrixError(
            f'{path}:{lineno}: expected {len(_FIELD_NAMES)} fields, '
            f'got {len(raw_fields)}: {line!r}')

    values = dict(zip(_FIELD_NAMES, raw_fields))
    for name in _INT_FIELDS:
        try:
            values[name] = int(values[name])
        except ValueError as exc:
            raise MatrixError(
                f'{path}:{lineno}: {name!r} is not an integer: '
                f'{values[name]!r}') from exc

    cell = Cell(**values)
    _validate(path, lineno, cell)
    return cell


def _validate(path, lineno, cell):
    if not _ID_RE.match(cell.id):
        raise MatrixError(
            f'{path}:{lineno}: id {cell.id!r} is not [a-z0-9_]+')
    if cell.guest not in _VALID_GUESTS:
        raise MatrixError(
            f'{path}:{lineno}: unknown guest {cell.guest!r}')
    if cell.speed not in _VALID_SPEEDS:
        raise MatrixError(
            f'{path}:{lineno}: unknown speed {cell.speed!r}')
    if cell.accel not in _VALID_ACCELS:
        raise MatrixError(
            f'{path}:{lineno}: unknown accel {cell.accel!r}')
    if cell.expect not in _VALID_EXPECTS:
        raise MatrixError(
            f'{path}:{lineno}: unknown expect {cell.expect!r}')
    if cell.expect == 'xfail' and not (cell.xfail_oracle and
                                        cell.xfail_signature):
        raise MatrixError(
            f'{path}:{lineno}: xfail cell {cell.id!r} needs both '
            'xfail_oracle and xfail_signature')
