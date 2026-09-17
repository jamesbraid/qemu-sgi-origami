# Screendump-based pass conditions: reading a captured PPM framebuffer and
# judging it against bounded, general checks
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A graphics device's pass condition cannot be a console-text match: the
thing under test is what landed in the framebuffer, and the whole point of
a screendump check is to look at that instead. This module reads the PPM
QEMU's own `screendump` QMP/HMP command writes and judges it with two
bounded, general checks rather than an exact pixel-for-pixel golden image:

  - NotAllBlackCheck: is any pixel above a small threshold at all. This is
    the coarsest possible check and exactly what would have caught RAD4's
    real BAR2-unreachable regression (docs/research/graphics/
    rad4-xradsgi-connection-hang-20260916.md in the Origami checkout):
    with that gap in place, every framebuffer byte a booted guest produced
    stayed zero for the whole run, confirmed live against the pre-fix
    revision (QEMU commit c9d4b3d6e2's parent) while developing this
    check -- screendump there yields exactly 0 non-zero bytes out of
    1280x1024x3.
  - RegionColorCheck: does a named rectangle's average color fall inside a
    recorded range. Loose enough to tolerate ordinary rendering variance
    (anti-aliasing, minor timing-dependent partial redraws at the moment
    the dump was taken), tight enough to catch a real channel/scaling bug
    -- the kind of near-black, blue-channel-only image RAD4 produced
    before QEMU commit e63c96c1a4 fixed the RGB561 color LUT port would
    fail any check whose range requires a real gray or white.

Deliberately not attempted here: exact pixel-for-pixel comparison against a
saved reference image. This project's own terminology reserves "golden" for
an exact transcript; screen rendering is not bit-exact across QEMU versions
or emulated timing, so a golden-image check on a screendump would be
fragile in exactly the way a golden is not supposed to be. Region-average
color is the general mechanism this module offers instead; a future check
that genuinely needs more (a shape, a glyph) is a new check class here, not
a reason to start comparing whole images byte for byte.

Every check class below takes (width, height, data) rather than a file
path, so it can be unit-tested against synthesized pixel data with no
screendump and no VM (see test_lib_screen.py); read_ppm() and
evaluate_screendump() are the only two functions that touch a real file.
"""

import dataclasses


class ScreenCheckError(Exception):
    """Raised when a screendump file cannot be read as the PPM format
    QEMU's screendump command produces (P6, 8 bits per channel)."""


def read_ppm(path):
    """Read a binary PPM (P6) file into (width, height, data).

    `data` is the raw row-major RGB byte sequence, three bytes per pixel,
    matching QEMU's own screendump output (docs/ui.json: 24-bit RGB,
    8 bits per channel, default format 'ppm'). Raises ScreenCheckError on
    anything other than an 8-bit P6 file -- a truncated or unexpected
    capture must fail loudly rather than be silently misread as a
    black image.
    """
    with open(path, 'rb') as fobj:
        magic = fobj.readline().strip()
        if magic != b'P6':
            raise ScreenCheckError(
                f'{path}: expected a P6 PPM, got magic {magic!r}')
        dims = fobj.readline().split()
        if len(dims) != 2:
            raise ScreenCheckError(f'{path}: malformed PPM dimensions line')
        width, height = (int(value) for value in dims)
        maxval = int(fobj.readline())
        if maxval != 255:
            raise ScreenCheckError(
                f'{path}: expected an 8-bit PPM (maxval 255), got {maxval}')
        data = fobj.read(width * height * 3)
        if len(data) != width * height * 3:
            raise ScreenCheckError(
                f'{path}: truncated PPM: expected {width * height * 3} '
                f'pixel bytes, got {len(data)}')
    return width, height, data


def _region_average(width, data, rect):
    """The (r, g, b) average of the rectangle (x, y, w, h) in data.

    Raises ScreenCheckError if the rectangle reaches outside the image --
    a check whose rectangle no longer fits (a resolution change) must
    fail with a clear reason, not silently average a truncated or wrong
    slice.
    """
    x, y, rw, rh = rect
    height = len(data) // (width * 3)
    if x < 0 or y < 0 or x + rw > width or y + rh > height:
        raise ScreenCheckError(
            f'region {rect!r} does not fit inside a {width}x{height} image')
    red = green = blue = 0
    for row in range(y, y + rh):
        offset = row * width * 3 + x * 3
        line = data[offset:offset + rw * 3]
        red += sum(line[0::3])
        green += sum(line[1::3])
        blue += sum(line[2::3])
    count = rw * rh
    return red / count, green / count, blue / count


@dataclasses.dataclass(frozen=True)
class NotAllBlackCheck:
    """Fails only when every channel of every pixel is at or below
    `threshold` -- the coarsest possible screen-content check, and the one
    that would have caught RAD4's BAR2-unreachable regression (see this
    module's own docstring). `threshold` defaults to a few counts of
    headroom above exact zero rather than 0 itself, so a single stray bit
    from an unrelated source (e.g. host display-server rounding, were this
    ever run against a non-headless backend) cannot fail a check whose
    subject is "did the guest paint anything at all", not "is any byte
    nonzero".
    """
    threshold: int = 4

    def evaluate(self, width, height, data):
        peak = max(data) if data else 0
        if peak > self.threshold:
            return True, f'peak channel value {peak} > {self.threshold}'
        return False, (
            f'every pixel channel is <= {self.threshold} '
            f'(peak {peak}) across the whole {width}x{height} frame -- '
             'the screen is black')


@dataclasses.dataclass(frozen=True)
class RegionColorCheck:
    """Fails unless rect's average (r, g, b) falls within
    [min_rgb, max_rgb] on every channel. `name` identifies the region in a
    failure message (e.g. 'login-dialog-chrome'); `rect` is (x, y, w, h) in
    screendump pixel coordinates.
    """
    name: str
    rect: tuple
    min_rgb: tuple
    max_rgb: tuple

    def evaluate(self, width, height, data):
        del height  # only used by _region_average via len(data)
        average = _region_average(width, data, self.rect)
        in_range = all(lo <= value <= hi for value, lo, hi in
                       zip(average, self.min_rgb, self.max_rgb))
        rounded = tuple(round(value, 1) for value in average)
        if in_range:
            return True, f'{self.name} averaged {rounded}, inside range'
        return False, (
            f'{self.name} averaged {rounded}, outside the expected range '
            f'{self.min_rgb}-{self.max_rgb}')


def evaluate_screendump(path, checks):
    """Read the PPM at path and run every check against it.

    Returns (all_passed, [message, ...]) -- one message per check,
    passing or failing, so a caller can log the whole picture rather than
    just the first failure. Raises ScreenCheckError (uncaught) if the file
    itself cannot be read as a PPM; that is an infrastructure failure, not
    a check result, and callers should treat it the same way a missing
    asset is treated elsewhere in this directory -- distinct from an
    ordinary failed check.
    """
    width, height, data = read_ppm(path)
    messages = []
    passed = True
    for check in checks:
        ok, message = check.evaluate(width, height, data)
        messages.append(message)
        passed = passed and ok
    return passed, messages
