# The Origin 200 test matrix: manifest format and parser
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The matrix manifest is a pipe-delimited table, one cell per line: `#`
comments and blank lines are ignored, and each row's pipe-separated
fields follow the exact order of the Cell dataclass below.
parse_matrix() turns a manifest into Cell records; machine_props() and
qemu_args() turn a Cell into the -M value and the rest of the QEMU
command line; prom_args() supplies the -bios and trace arguments every
cell's real PROM boot needs (the test class supplies the image);
pass_condition_met() decides whether a captured boot log satisfies the
recorded firmware progress mark; verdict() turns a cell and its
recorded AttemptResults into a
pass/fail/xfail/xpass/error outcome. Later stages of this subsystem
(the generic test body and the meson projection) build on all of
these.
"""

import re
from dataclasses import dataclass, fields
from pathlib import Path

from lib_prom import MARK, codes_seen, mark_reached


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
    speed: str
    timeout: int
    attempts: int
    min_passes: int
    expect: str
    xfail_mark: str
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


def matrix_rows(path):
    """Every data row of a manifest, as (lineno, line) pairs.

    Comments and blank lines are dropped and nothing is parsed, so this
    cannot fail on a bad row: a caller that wants one test per row can
    enumerate the rows here and leave parse_row(), which can fail, to
    each row's own test.
    """
    rows = []
    lines = Path(path).read_text(encoding='utf-8').splitlines()
    for lineno, raw_line in enumerate(lines, start=1):
        line = raw_line.strip()
        if not line or line.startswith('#'):
            continue
        rows.append((lineno, line))
    return rows


def parse_matrix(path):
    """Parse a pipe-delimited matrix manifest into a list of Cell rows.

    Raises MatrixError on any row that fails to split into the
    expected number of fields, or fails a value check.
    """
    cells = []
    seen_ids = set()
    for lineno, line in matrix_rows(path):
        cell = parse_row(path, lineno, line)
        if cell.id in seen_ids:
            raise MatrixError(
                f'{path}:{lineno}: duplicate cell id {cell.id!r}')
        seen_ids.add(cell.id)
        cells.append(cell)
    return cells


def peek_row(line):
    """A row's id and speed fields read raw, before any parsing: (id, speed).

    For naming and routing a test before parse_row() has judged the row.
    Either value is None when the row cannot supply a usable one -- the
    field count is wrong, the id is not [a-z0-9_]+, or the speed is not
    one of the known speeds -- so a caller can give such a row a
    placeholder name and route it wherever a bad row should fail. Every
    check parse_row() makes is still made by parse_row(); this reads
    two fields and validates nothing else.
    """
    raw_fields = [field.strip() for field in line.split('|')]
    if len(raw_fields) != len(_FIELD_NAMES):
        return None, None
    values = dict(zip(_FIELD_NAMES, raw_fields))
    cell_id = values['id'] if _ID_RE.match(values['id']) else None
    speed = values['speed'] if values['speed'] in _VALID_SPEEDS else None
    return cell_id, speed


def parse_row(path, lineno, line):
    """Parse and validate one manifest row into a Cell.

    Raises MatrixError, naming path and lineno, on a wrong field count
    or a failed value check. The duplicate-id check needs the whole
    manifest and is parse_matrix()'s.
    """
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
    if cell.expect == 'xfail' and not (cell.xfail_mark and
                                        cell.xfail_signature):
        raise MatrixError(
            f'{path}:{lineno}: xfail cell {cell.id!r} needs both '
            'xfail_mark and xfail_signature')


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

# Regimes that run single-threaded round-robin TCG and so need the kick
# period pinned once more than one CPU shares that one thread.
_ROUND_ROBIN_ACCELS = {'wallclock', 'roundrobin'}

# QEMU's own default (100 ms) is documented in test_sn0_smp.py as the
# negative control for a cross-CPU rendezvous with a real-time deadline --
# exactly the shape of the stock PROM's discovery and rendezvous protocol.
# Every launcher that boots the real PROM at more than one CPU under this
# regime already pins this same value (tools/origami_lib/sash.py's
# DEFAULT_RR_KICK_PERIOD_NS); a matrix cell that boots the real PROM at
# more than one CPU needs it too, or it inherits the negative control.
ROUND_ROBIN_KICK_PERIOD_NS = 100000

_MEM_RE = re.compile(r'^(\d+)([A-Za-z]*)$')


def machine_props(cell):
    """Build the -M origin200,... machine property string for a cell.

    topology= names the wiring a multi-node cell needs; every cell in
    the manifest boots the real PROM, the only firmware the machine has.
    """
    if cell.nodes == 1:
        return 'origin200,debug-leds=on'
    return 'origin200,topology=origin200-dual,debug-leds=on'


def qemu_args(cell):
    """Build the QEMU args that follow -M for a cell: accelerator (plus
    icount, for the icount regime, or the round-robin kick period, for
    wallclock/roundrobin above one CPU), -m and -smp. A multi-node cell
    splits -m evenly across the nodes topology= already named -- -numa
    is refused, so there is no per-node memory backend to build here.
    """
    args = list(_ACCEL_ARGS[cell.accel])
    if cell.accel in _ROUND_ROBIN_ACCELS and cell.cpus > 1:
        args[1] += f',rr-kick-period-ns={ROUND_ROBIN_KICK_PERIOD_NS}'
    args += ['-m', cell.mem]

    if cell.nodes == 1:
        args += ['-smp', str(cell.cpus)]
        return args

    if cell.cpus % cell.nodes != 0:
        raise MatrixError(
            f'cell {cell.id!r}: cpus {cell.cpus} does not split evenly '
            f'across {cell.nodes} nodes')
    args += ['-smp', str(cell.cpus)]

    # Fail fast in Python, matching the machine's own even-split check.
    _split_mem(cell.mem, cell.nodes)

    return args


def prom_args(firmware_path):
    """The -bios/-d args every cell needs to run the real PROM.

    Given a resolved path, returns ['-bios', firmware_path,
    '-d', 'trace:sgi_hub_led'] -- both parts matter: -bios hands the
    machine the real firmware, and the trace event is what emits the
    progress codes pass_condition_met() reads. Given a falsy path (the
    image is not provisioned under QEMU_TEST_SGI_ASSET_DIR), returns []. Callers
    can call this unconditionally rather than guarding it with an if, and
    let the missing -bios show up as a boot that never emits a progress
    code rather than a KeyError building the argv. Matches
    test_prom_boot.py's own vm.add_args() call.
    """
    if not firmware_path:
        return []
    return ['-bios', firmware_path, '-d', 'trace:sgi_hub_led']


@dataclass
class AttemptResult:
    """The outcome of one boot attempt against a cell's pass condition.

    reached_pass is True when the attempt observed its expected
    success signal (a progress code, exit marker, or console pattern,
    depending on the cell's pass condition). signature is the failure
    signature recorded when it did not, for matching against an xfail
    cell's xfail_signature. infra_error is set instead of either when
    the attempt could not run to a real verdict at all — a missing
    asset, a QEMU crash, a timeout with no signature captured.
    """
    reached_pass: bool
    signature: str | None = None
    infra_error: str | None = None


def verdict(cell, attempts):
    """Decide a cell's verdict from its recorded AttemptResults.

    Returns one of 'pass', 'fail', 'xfail', 'xpass', 'error'.

    An infra_error on any attempt always wins: it is never absorbed
    into a pass or an xfail, because it means the attempt never
    produced a real answer about the guest. Otherwise a 'pass' cell
    passes once at least min_passes attempts reached their pass
    condition, and an 'xfail' cell passes (as 'xfail') only when every
    attempt that missed its pass condition failed with exactly the
    recorded xfail_signature — reaching the pass condition at all is
    an 'xpass', and missing it with a different signature is a real
    'fail'.
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
        reached = sum(1 for attempt in attempts if attempt.reached_pass)
        return 'pass' if reached >= cell.min_passes else 'fail'

    # cell.expect == 'xfail'
    if all(attempt.reached_pass for attempt in attempts):
        return 'xpass'
    signatures = {attempt.signature for attempt in attempts
                  if not attempt.reached_pass}
    if signatures == {cell.xfail_signature}:
        return 'xfail'
    return 'fail'


def pass_condition_met(_scenario, log_text):
    """Whether log_text (a captured boot log) satisfies the pass
    condition: the recorded firmware-boot progress mark (lib_prom.MARK)
    is a SUBSET of the codes seen in log_text -- lib_prom's own
    mark_reached(codes_seen(log_text), MARK). A boot that wedges after
    an early code never reaches the mark and so never satisfies this;
    the timeout path stays the caller's job. Pure text-in, bool-out --
    unit-testable without a boot.

    `scenario` used to select a second, weaker condition (a 'smoke'
    scenario, any progress code at all, for cells with no firmware
    image to run) -- gone with the synthesized-firmware axis that was
    its only use, so every cell now takes this one path regardless of
    what its scenario names. Kept as a parameter for call-site
    compatibility with what scores a cell's log.
    """
    reached, _missing = mark_reached(codes_seen(log_text), MARK)
    return reached


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
