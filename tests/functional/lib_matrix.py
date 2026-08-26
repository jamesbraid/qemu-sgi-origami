# The Origin 200 test matrix: manifest format and parser
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The matrix manifest is a pipe-delimited table, one cell per line: `#`
comments and blank lines are ignored, and each row's pipe-separated
fields follow the exact order of the Cell dataclass below.
parse_matrix() turns a manifest into Cell records; machine_props() and
qemu_args() turn a Cell into the -M value and the rest of the QEMU
command line; resolve_prom() and prom_args() supply the real PROM
image a firmware=prom cell's -M string demands (a firmware=synth cell
needs neither); verdict() turns a cell and its recorded AttemptResults
into a pass/fail/xfail/xpass/error outcome. Later stages of this
subsystem (the generic test body and the meson projection) build on
all four.
"""

import os
import re
from dataclasses import dataclass, fields
from pathlib import Path


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
_VALID_FIRMWARE = {'synth', 'prom'}
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
    if cell.firmware not in _VALID_FIRMWARE:
        raise MatrixError(
            f'{path}:{lineno}: unknown firmware {cell.firmware!r}')
    if cell.expect not in _VALID_EXPECTS:
        raise MatrixError(
            f'{path}:{lineno}: unknown expect {cell.expect!r}')
    if cell.expect == 'xfail' and not (cell.xfail_oracle and
                                        cell.xfail_signature):
        raise MatrixError(
            f'{path}:{lineno}: xfail cell {cell.id!r} needs both '
            'xfail_oracle and xfail_signature')


# The two accelerator regimes a cell's accel field may select between:
# icount is the only one that runs deterministically (single-threaded
# TCG with instruction-counted virtual time), mttcg is real
# multi-threaded TCG, and wallclock/roundrobin both run single-threaded
# TCG against the host clock, without icount.
_ACCEL_ARGS = {
    'icount': ['-accel', 'tcg,thread=single',
               '-icount', 'shift=2,align=off,sleep=on'],
    'mttcg': ['-accel', 'tcg,thread=multi'],
    'wallclock': ['-accel', 'tcg,thread=single'],
    'roundrobin': ['-accel', 'tcg,thread=single'],
}

_MEM_RE = re.compile(r'^(\d+)([A-Za-z]*)$')


def machine_props(cell):
    """Build the -M origin200,... machine property string for a cell."""
    synthesized = 'on' if cell.firmware == 'synth' else 'off'
    return f'origin200,synthesized-firmware={synthesized},debug-leds=on'


def qemu_args(cell):
    """Build the QEMU args that follow -M for a cell: accelerator (plus
    icount, for the icount regime), -m, -smp, and, for a multi-node
    cell, one memory-backend-ram object and one -numa node per node.
    """
    args = list(_ACCEL_ARGS[cell.accel])
    args += ['-m', cell.mem]

    if cell.nodes == 1:
        args += ['-smp', str(cell.cpus)]
        return args

    if cell.cpus % cell.nodes != 0:
        raise MatrixError(
            f'cell {cell.id!r}: cpus {cell.cpus} does not split evenly '
            f'across {cell.nodes} nodes')
    cpus_per_node = cell.cpus // cell.nodes
    args += ['-smp',
             f'{cell.cpus},sockets={cell.nodes},'
             f'cores={cpus_per_node},threads=1']

    per_node_mem = _split_mem(cell.mem, cell.nodes)
    for node in range(cell.nodes):
        args += ['-object',
                 f'memory-backend-ram,id=ram{node},size={per_node_mem}']
    for node in range(cell.nodes):
        lo = node * cpus_per_node
        hi = lo + cpus_per_node - 1
        args += ['-numa',
                 f'node,nodeid={node},memdev=ram{node},cpus={lo}-{hi}']

    return args


# Where ORIGAMI_ASSETS resolves the real PROM image for a firmware=prom
# cell, matching test_prom_boot.py's own FIRMWARE_REL exactly -- both name
# the same image, so the path is kept here rather than re-derived twice.
FIRMWARE_REL_PATH = os.path.join('assets', 'firmware', '6.5-base',
                                 'ip27prom.payload.bin')


def resolve_prom(assets_root):
    """The real PROM image's path under assets_root, or None.

    None when assets_root is falsy (ORIGAMI_ASSETS unset) or the image
    is not there -- the runner may not have it provisioned yet. Purely
    a path check: callers resolve ORIGAMI_ASSETS from the environment
    themselves and hand the result in, matching test_prom_boot.py's own
    _firmware_path(). The caller decides what an unresolved image means
    for the cell (test_origin200_matrix.py skips it); this function only
    answers whether the image is there.
    """
    if not assets_root:
        return None
    path = os.path.join(assets_root, FIRMWARE_REL_PATH)
    return path if os.path.isfile(path) else None


def prom_args(firmware_path):
    """The -bios/-d args a firmware=prom cell needs to run the real PROM.

    Given a resolved path, returns ['-bios', firmware_path,
    '-d', 'trace:sgi_hub_led'] -- both parts matter: -bios hands the
    machine the real firmware, and the trace event is what emits the
    progress codes first_progress_code() reads. Given a falsy path
    (None, for a firmware=synth cell that never resolves one), returns
    [] -- callers can call this unconditionally rather than guarding it
    with an if. Matches test_prom_boot.py's own vm.add_args() call.
    """
    if not firmware_path:
        return []
    return ['-bios', firmware_path, '-d', 'trace:sgi_hub_led']


@dataclass
class AttemptResult:
    """The outcome of one boot attempt against a cell's oracle.

    reached_oracle is True when the attempt observed its expected
    success signal (a progress code, exit marker, or console pattern,
    depending on the cell's oracle). signature is the failure
    signature recorded when it did not, for matching against an xfail
    cell's xfail_signature. infra_error is set instead of either when
    the attempt could not run to a real verdict at all — a missing
    asset, a QEMU crash, a timeout with no signature captured.
    """
    reached_oracle: bool
    signature: str | None = None
    infra_error: str | None = None


def verdict(cell, attempts):
    """Decide a cell's verdict from its recorded AttemptResults.

    Returns one of 'pass', 'fail', 'xfail', 'xpass', 'error'.

    An infra_error on any attempt always wins: it is never absorbed
    into a pass or an xfail, because it means the attempt never
    produced a real answer about the guest. Otherwise a 'pass' cell
    passes once at least min_passes attempts reached the oracle, and
    an 'xfail' cell passes (as 'xfail') only when every attempt that
    missed the oracle failed with exactly the recorded xfail_signature
    — reaching the oracle at all is an 'xpass', and missing it with a
    different signature is a real 'fail'.
    """
    if not attempts:
        raise MatrixError(
            f'cell {cell.id!r}: verdict() needs at least one attempt')

    # infra_error is SET when it is not None, not merely truthy: a
    # recorded empty string is still a recorded infra error, distinct
    # from the None a clean attempt leaves it at.
    if any(attempt.infra_error is not None for attempt in attempts):
        return 'error'

    if cell.expect == 'pass':
        reached = sum(1 for attempt in attempts if attempt.reached_oracle)
        return 'pass' if reached >= cell.min_passes else 'fail'

    # cell.expect == 'xfail'
    if all(attempt.reached_oracle for attempt in attempts):
        return 'xpass'
    signatures = {attempt.signature for attempt in attempts
                  if not attempt.reached_oracle}
    if signatures == {cell.xfail_signature}:
        return 'xfail'
    return 'fail'


_PROGRESS_CODE_RE = re.compile(r'sgi_hub_led CPU \d+: (0x[0-9a-f]{2}) \((\w+):')


def first_progress_code(text):
    """Return the first firmware boot-progress code in text, or None.

    The Hub's debug-leds trace event prints a line of the form
    'sgi_hub_led CPU N: 0xNN (KIND: meaning)' for every write to its
    indicator register, where KIND is 'progress', 'FAILURE' (a
    diagnosed self-test failure such as ECC or FPU), or 'undecoded'.
    A failure line has the identical shape a progress line does, so
    this only ever returns a code whose kind is explicitly 'progress'
    -- scoring a FAILURE code as progress would be a false pass for a
    cell that actually hit one -- skipping past any failure or
    undecoded line that appears first.
    """
    for match in _PROGRESS_CODE_RE.finditer(text):
        code, kind = match.group(1), match.group(2)
        if kind == 'progress':
            return code
    return None


def _split_mem(mem, nodes):
    """Split a memory size string (e.g. '128M') evenly across nodes."""
    match = _MEM_RE.match(mem)
    if not match:
        raise MatrixError(f'unrecognised mem size {mem!r}')
    value, suffix = match.groups()
    value = int(value)
    if value % nodes != 0:
        raise MatrixError(
            f'mem {mem!r} does not split evenly across {nodes} nodes')
    return f'{value // nodes}{suffix}'
