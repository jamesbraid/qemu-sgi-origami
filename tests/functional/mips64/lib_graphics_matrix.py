# The graphics test matrix: manifest format, scenario registry and parser
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
origin200-matrix's own Cell format (id | guest | scenario | cpus | nodes |
mem | accel | speed | timeout | attempts | min_passes | expect | xfail_mark
| xfail_signature, all parsed by lib_matrix.py) has no room for what a
graphics device's functional test needs to say: which extra `-device` to
attach, which disk image to boot, and what "the screen looks right" means
for this scenario. Stretching that one row shape to cover screen checks
too -- adding a column that has to hold a rectangle, an RGB range and an
input script as flat text -- would make an already-dense row unreadable
for a net gain of nothing, since origin200-matrix's own rows would have to
carry the same new, unused columns. So this is a second, sibling manifest
(graphics-matrix) and a second Cell shape (GraphicsCell), reusing every
piece of lib_matrix.py that does not assume the PROM-progress-code shape:
matrix_rows() (the raw pipe-row reader, already row-shape-agnostic),
AttemptResult, MatrixError and verdict() (all written against attribute
access, not the Cell class itself, so a GraphicsCell with the same id/
expect/min_passes/xfail_signature attributes satisfies them unchanged),
and qemu_args() (reads only cell.accel/cpus/mem/nodes -- GraphicsCell
carries a fixed nodes=1, since no graphics scenario here needs NUMA
topology). Only what genuinely differs -- the row shape, its validation,
and what a screen-check scenario actually does -- is new.

A screen-check scenario is named symbolically from the manifest's
`scenario` column and defined once in SCENARIOS below, the same way
origin200-matrix's pass condition (lib_prom.MARK, a fixed set of progress
codes) is Python data the manifest only refers to by name rather than
spelling out inline. Adding a new device's row means adding a Scenario
entry here (console text to wait for, an optional lib_input.run_script()
script, and a list of lib_screen check objects) and one manifest line
naming it -- no changes to the parsing or attempt-running code below.
"""

import os
import re
from dataclasses import dataclass

from lib_matrix import MatrixError
from lib_screen import NotAllBlackCheck, RegionColorCheck


class GraphicsMatrixError(MatrixError):
    """Raised for any graphics-matrix row that fails to parse or
    validate. Subclasses MatrixError so a caller that already handles
    MatrixError from the PROM matrix catches this too."""


@dataclass
class GraphicsCell:
    id: str
    guest: str
    device: str
    disk: str
    scenario: str
    cpus: int
    mem: str
    accel: str
    speed: str
    timeout: int
    attempts: int
    min_passes: int
    expect: str
    xfail_mark: str
    xfail_signature: str
    # Every graphics scenario below boots one node; this is not a manifest
    # column (see this module's own docstring) but qemu_args() from
    # lib_matrix reads cell.nodes, so it needs to exist.
    nodes: int = 1


# The manifest's field order. Kept as an explicit list, unlike lib_matrix's
# _FIELD_NAMES (derived from Cell's own fields), because GraphicsCell
# carries the extra `nodes` field above that has no column of its own.
_FIELD_NAMES = ['id', 'guest', 'device', 'disk', 'scenario', 'cpus', 'mem',
               'accel', 'speed', 'timeout', 'attempts', 'min_passes',
               'expect', 'xfail_mark', 'xfail_signature']
_INT_FIELDS = {'cpus', 'timeout', 'attempts', 'min_passes'}

_ID_RE = re.compile(r'^[a-z0-9_]+$')
# Only IRIX has a scenario in SCENARIOS below that can drive a guest UI to
# a screen-check point; unlike lib_matrix's origin200-matrix (which also
# validates 'linux' and 'openbsd' rows against the real PROM), there is no
# non-IRIX graphics scenario yet, so the valid set stays narrow rather than
# accepting a guest value nothing here can run.
_VALID_GUESTS = {'irix'}
_VALID_SPEEDS = {'quick', 'thorough'}
_VALID_ACCELS = {'icount', 'mttcg', 'wallclock', 'roundrobin'}
_VALID_EXPECTS = {'pass', 'xfail'}

# The installed-IRIX disk asset convention test_irix_diag.py and
# test_irix_installed_boot.py already use: irix/<release>/
# <variant>.qcow2 under QEMU_TEST_SGI_ASSET_DIR, resolved with
# lib_prom.resolve_asset
# exactly as those files do. A graphics-matrix row's `disk` column is just
# <variant>; disk_rel() below builds the same relative path those files'
# own DISK_REL constants build by hand.
RELEASE = '6.5.30'


def disk_rel(variant):
    """Return the asset-relative path of an installed-IRIX disk variant.

    QEMU_TEST_SGI_ASSET_DIR is the asset root. For example, disk_rel('root-rad4') -> 'irix/6.5.30/
    root-rad4.qcow2'. Resolve it with lib_prom.resolve_asset the same way
    every other disk-booting test in this directory resolves its own
    DISK_REL."""
    return os.path.join('irix', RELEASE, f'{variant}.qcow2')


@dataclass
class Scenario:
    """What one named graphics scenario does, once its guest has booted.

    `console_wait` is a bytes regex lib_prom.drive_console's `until`
    argument waits for on the console before anything else happens --
    for every scenario here, IRIX's own login(1) prompt. The serial
    getty that prints it and xdm's own X server are two independent
    processes racing out of the same multiuser boot, not synchronized by
    anything visible on the console, so this text match is a necessary
    but not sufficient readiness signal: lib_screen's
    poll_screendump_until_checks_pass() absorbs the remaining gap by
    polling the screendump itself, rather than this field promising a
    stronger guarantee than a console-text match can actually make.
    `input_steps` is a lib_input.run_script() step list (empty for a
    scenario that only needs to look at the screen as booted).
    `screen_checks` is a list of lib_screen check objects (NotAllBlackCheck,
    RegionColorCheck, ...) evaluated against the screendump taken once
    input_steps has run and the checked region has settled.
    """
    console_wait: bytes
    input_steps: list
    screen_checks: list


# RAD4's own login-screen scenario. The region and expected range below
# were measured live against this fork's own tip while building this
# test (a2546528e2 and successors): booting IRIX 6.5.30 with
# `-device psitech-rad4,addr=5` (the vendor-documented slot), waiting for
# "... console login:" on the plain text console, then polling the screendump
# until xdm's own, independently-timed redraw catches up, shows a
# screendump with the rendered login screen; the plain gray area of that
# dialog (below the four user-face icons, above the login-name field)
# reads (217, 217, 217) there. Before it settles, a screendump can still
# show only the root window's stipple pattern (confirmed live: this
# scenario's own region then averages close to (64, 64, 64), the
# stipple's own measured value, not the dialog's). The input steps below
# (a harmless modifier keypress and a chunked mouse move) exercise
# lib_input's helpers end to end without changing what is on screen, so
# they run before the screen checks rather than after.
SCENARIOS = {
    'rad4_login_screen': Scenario(
        console_wait=rb'login:',
        input_steps=[
            ('key', 'shift'),
            ('wait', 0.2),
            ('move', 1800, -1300),
            ('wait', 0.2),
        ],
        screen_checks=[
            NotAllBlackCheck(),
            RegionColorCheck('login-dialog-chrome', rect=(400, 450, 150, 60),
                             min_rgb=(190, 190, 190), max_rgb=(235, 235, 235)),
        ],
    ),
}


def peek_row(line):
    """A row's id and speed fields read raw, before any parsing: (id, speed).

    Mirrors lib_matrix.peek_row() exactly, for the same reason: naming and
    routing a test method before parse_row() has judged the row."""
    raw_fields = [field.strip() for field in line.split('|')]
    if len(raw_fields) != len(_FIELD_NAMES):
        return None, None
    values = dict(zip(_FIELD_NAMES, raw_fields))
    cell_id = values['id'] if _ID_RE.match(values['id']) else None
    speed = values['speed'] if values['speed'] in _VALID_SPEEDS else None
    return cell_id, speed


def parse_row(path, lineno, line):
    """Parse and validate one graphics-matrix row into a GraphicsCell.

    Mirrors lib_matrix.parse_row(); the differences are the field list
    (GraphicsCell's, not Cell's) and the extra checks for `device`,
    `disk` and `scenario`."""
    raw_fields = [field.strip() for field in line.split('|')]
    if len(raw_fields) != len(_FIELD_NAMES):
        raise GraphicsMatrixError(
            f'{path}:{lineno}: expected {len(_FIELD_NAMES)} fields, '
            f'got {len(raw_fields)}: {line!r}')

    values = dict(zip(_FIELD_NAMES, raw_fields))
    for name in _INT_FIELDS:
        try:
            values[name] = int(values[name])
        except ValueError as exc:
            raise GraphicsMatrixError(
                f'{path}:{lineno}: {name!r} is not an integer: '
                f'{values[name]!r}') from exc

    cell = GraphicsCell(**values)
    _validate(path, lineno, cell)
    return cell


def _validate(path, lineno, cell):
    if not _ID_RE.match(cell.id):
        raise GraphicsMatrixError(
            f'{path}:{lineno}: id {cell.id!r} is not [a-z0-9_]+')
    if cell.guest not in _VALID_GUESTS:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: unknown guest {cell.guest!r}')
    if not cell.device:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: cell {cell.id!r} has an empty device column')
    if not cell.disk:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: cell {cell.id!r} has an empty disk column')
    if cell.scenario not in SCENARIOS:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: unknown scenario {cell.scenario!r} -- add '
            'it to lib_graphics_matrix.SCENARIOS')
    if cell.speed not in _VALID_SPEEDS:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: unknown speed {cell.speed!r}')
    if cell.accel not in _VALID_ACCELS:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: unknown accel {cell.accel!r}')
    if cell.expect not in _VALID_EXPECTS:
        raise GraphicsMatrixError(
            f'{path}:{lineno}: unknown expect {cell.expect!r}')
    if cell.expect == 'xfail' and not (cell.xfail_mark and
                                        cell.xfail_signature):
        raise GraphicsMatrixError(
            f'{path}:{lineno}: xfail cell {cell.id!r} needs both '
            'xfail_mark and xfail_signature')
