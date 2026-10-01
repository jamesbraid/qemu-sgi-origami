# Per-board feature matrix for the SI/MGRAS family's IRIX desktop boots
#
# SPDX-License-Identifier: BSD-3-Clause
"""
The boards, the programs that exercise each drawing feature, and the
colour-statistics comparison the per-board boot tests use. Everything
here is plain Python with no QEMU dependency so test_lib_mgras_matrix.py
can run it in seconds.

A feature is shown by running one stock IRIX program on the desktop and
comparing the screendump with a reference by colour statistics. Exact
pixels are the wrong comparison: the pointer, window placement and the
program's own animation phase change from run to run, so the check asks
three questions of the whole frame instead.

  - Did the program draw at all? A fraction of pixels must differ from
    the plain desktop captured just before it started.
  - Are the dominant colours the ones the program paints? Every colour
    the reference lists with at least MIN_REFERENCE_FRACTION of the
    frame must lie within a tolerance of one of the capture's most
    frequent colours.
  - Is the mean colour close? Within MEAN_TOLERANCE per channel.

The reference is a small JSON file of those statistics, not a frame, so
it is cheap to commit and review. write_stats() produces one from a
screendump; each run saves its own candidate in the log directory so a
reference can be taken from a run that was checked by eye. See
reference_path() for where a file goes.

Reference statistics have to come from an emulator run that was judged
correct, which needs the IRIX disk and a build, so none are committed
yet. Until a reference exists the structural check (the program drew,
over a desktop that was already up) still runs, and the missing
reference is logged. Setting QEMU_TEST_MGRAS_REQUIRE_REFERENCES=1 turns
a missing reference into a failure, for the run that is meant to prove
them all present.
"""

import dataclasses
import json
import math
import os

from lib_screen import dominant_colors

HERE = os.path.dirname(os.path.abspath(__file__))
REFERENCE_DIR = os.path.join(HERE, 'mgras-reference')

# Colours the capture's top-N list is searched for each reference colour.
TOP_COLORS = 12
# A reference colour covering less of the frame than this is dither or
# antialiasing, not something the program must reproduce.
MIN_REFERENCE_FRACTION = 0.03
# How many dominant colours write_stats() records.
RECORDED_COLORS = 8
DOMINANT_TOLERANCE = 25.0
MEAN_TOLERANCE = 20.0
# Fraction of pixels that must differ from the bare desktop for the
# program to count as having drawn. A small window is a few percent of a
# 1280x1024 screen.
MIN_CHANGED_FRACTION = 0.02
# A channel difference below this is the pointer's anti-aliasing and
# dithering noise, not drawing.
CHANGE_THRESHOLD = 24


@dataclasses.dataclass(frozen=True)
class Board:
    """One sgi-mgras board= value and what hinv prints for it."""
    option: str
    hinv_name: str
    # Extra -device properties. si-t is not in the model's implemented
    # set (it has texture hardware the model does not fully draw), so it
    # needs x-unimplemented-board to be presented at all.
    properties: tuple = ()
    textured: bool = False
    double_buffered: bool = False


BOARDS = {
    'si': Board('si', 'SI'),
    'si-t': Board('si-t', 'SI with texture option',
                  properties=('x-unimplemented-board=on',), textured=True),
    'ssi': Board('ssi', 'SSI', double_buffered=True),
    'essi': Board('essi', 'ESSI', double_buffered=True),
}

# mxi and emxi are refused by the model, and the matrix leaves them out.


def device_args(board):
    """The -device argument that fits BOARD into IO3."""
    spec = ','.join(('sgi-mgras', 'slot=io3', f'board={board.option}',
                     *board.properties))
    return ('-device', spec)


@dataclasses.dataclass(frozen=True)
class Feature:
    """One representative program for one drawing feature."""
    name: str
    command: str          # run as root on the serial console
    process: str          # killall target once the frame is taken
    settle_seconds: float
    applies: str          # 'all', 'textured' or 'double_buffered'

    def runs_on(self, board):
        if self.applies == 'textured':
            return board.textured
        if self.applies == 'double_buffered':
            return board.double_buffered
        return True


# The programs run from the root shell against the display :0 session
# that guest logged in to. DISPLAY and XAUTHORITY are set once by
# DISPLAY_SETUP. The names are the stock demo programs; the commands
# first try the PATH and then the demo directory, because where an
# installed disk puts them has not been confirmed on root-si.qcow2.
DISPLAY_SETUP = ('setenv DISPLAY :0.0; '
                 'setenv XAUTHORITY /usr/people/guest/.Xauthority')


def _demo(name, args=''):
    tail = f' {args}' if args else ''
    return (f'( {name}{tail} || /usr/demos/bin/{name}{tail} ) '
            '>& /dev/null &')


FEATURES = (
    # The 2D desktop is checked by lib_mgras_regions before any of these
    # run, so it is not a program here.
    Feature('lit3d', _demo('amesh'), 'amesh', 20.0, 'all'),
    # A textured Inventor scene. The scene file is one the stock Inventor
    # demos ship; the exact path is unconfirmed on the disk.
    Feature('texture', _demo('ivview', '/usr/share/data/models/'
                             'Textures/earth.iv'),
            'ivview', 40.0, 'textured'),
    # Pixel transfer: i3dm writes and reads back pixel rectangles as it
    # draws. A snapshot of a desktop region would exercise the read path
    # alone but needs pointer interaction.
    Feature('pixels', _demo('i3dm'), 'i3dm', 30.0, 'all'),
    # clock redraws every second and swaps buffers, which only a board
    # with the second raster engine does in hardware.
    Feature('doublebuffer', _demo('clock'), 'clock', 20.0,
            'double_buffered'),
)


def features_for(board):
    return tuple(f for f in FEATURES if f.runs_on(board))


def reference_path(board_option, feature_name):
    """Where the committed reference for a board and feature lives."""
    return os.path.join(REFERENCE_DIR,
                        f'{board_option}-{feature_name}.json')


def write_stats(path, width, data):
    """Record a frame's whole-screen colour statistics as JSON."""
    height = len(data) // (width * 3)
    rect = (0, 0, width, height)
    count = width * height
    channels = [sum(data[c::3]) / count for c in range(3)]
    colors = [[list(color), round(fraction, 4)] for color, fraction in
              dominant_colors(width, data, rect, RECORDED_COLORS)]
    with open(path, 'w', encoding='utf-8') as out:
        json.dump({'schema': 1, 'size': [width, height],
                   'mean': [round(c, 2) for c in channels],
                   'dominant': colors}, out, indent=2, sort_keys=True)
        out.write('\n')


def load_stats(path):
    with open(path, encoding='utf-8') as src:
        stats = json.load(src)
    if stats.get('schema') != 1:
        raise ValueError(f'{path}: unknown reference schema')
    return stats


def changed_fraction(base, current):
    """Fraction of pixels whose largest channel difference exceeds
    CHANGE_THRESHOLD. BASE and CURRENT are (width, height, data)."""
    if base[:2] != current[:2]:
        raise ValueError('frames differ in size')
    data_a, data_b = base[2], current[2]
    changed = 0
    for i in range(0, len(data_a), 3):
        if (abs(data_a[i] - data_b[i]) > CHANGE_THRESHOLD or
                abs(data_a[i + 1] - data_b[i + 1]) > CHANGE_THRESHOLD or
                abs(data_a[i + 2] - data_b[i + 2]) > CHANGE_THRESHOLD):
            changed += 1
    return changed / (len(data_a) // 3)


def compare_stats(reference, width, data):
    """(passed, message) comparing a frame with a reference file's
    statistics. See the module docstring for the rule."""
    height = len(data) // (width * 3)
    rect = (0, 0, width, height)
    count = width * height
    mean = [sum(data[c::3]) / count for c in range(3)]
    found = [color for color, _fraction in
             dominant_colors(width, data, rect, TOP_COLORS)]
    parts = []
    passed = True
    for want, fraction in reference['dominant']:
        if fraction < MIN_REFERENCE_FRACTION:
            continue
        best = min(found, key=lambda color: math.dist(color, want))
        delta = math.dist(best, want)
        ok = delta <= DOMINANT_TOLERANCE
        passed = passed and ok
        parts.append(f'{tuple(want)} ({fraction:.0%}) nearest {best} '
                     f'delta {delta:.1f}{"" if ok else " FAIL"}')
    mean_delta = max(abs(a - b) for a, b in zip(mean, reference['mean']))
    mean_ok = mean_delta <= MEAN_TOLERANCE
    passed = passed and mean_ok
    parts.append(f'mean {tuple(round(c, 1) for c in mean)} vs '
                 f'{tuple(reference["mean"])} max delta {mean_delta:.1f}'
                 f'{"" if mean_ok else " FAIL"}')
    return passed, '; '.join(parts)
