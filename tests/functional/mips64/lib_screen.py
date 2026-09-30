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
    the coarsest possible check and detects a framebuffer with no lit
    pixels.
  - RegionColorCheck: does a named rectangle's average color fall inside a
    recorded range. Loose enough to tolerate ordinary rendering variance
    (anti-aliasing, minor timing-dependent partial redraws at the moment
    the dump was taken), tight enough to catch a real channel/scaling bug
    -- a range can also detect channel or scaling errors.

Deliberately not attempted here: exact pixel-for-pixel comparison against a
saved reference image. This project's own terminology reserves "golden" for
an exact transcript; screen rendering is not bit-exact across QEMU versions
or emulated timing, so a golden-image check on a screendump would be
fragile in exactly the way a golden is not supposed to be. Region-average
color is the general mechanism this module offers instead; a future check
that genuinely needs more (a shape, a glyph) is a new check class here, not
a reason to start comparing whole images byte for byte.

Two more general checks, for a region where a flat average cannot tell a
real bitmap from a solid fill of the same mean brightness:

  - ColorVarietyCheck: does a rectangle contain at least `min_colors`
    distinct (r, g, b) values. This distinguishes a patterned region
    from a solid fill with the same average color.
  - DarkPixelFractionCheck: does at least `min_fraction` of a rectangle's
    pixels have every channel at or below `max_channel`. This suits a
    light-background render where glyph text or chrome shows up as a
    sparse set of hard-edged dark pixels.
  - LitPixelFractionCheck: the opposite polarity -- does at least
    `min_fraction` of a rectangle's pixels have any channel above
    `min_channel`. This suits a dark-background render with bright
    foreground content (the SI greeter capture above: black background,
    a bright icon fill, bright glyph text). A paint regression that
    erased that content would make the region *more* uniformly black,
    not less, so a lower-bound dark-fraction check would not catch it --
    and neither would ColorVarietyCheck alone catch a near-total-loss
    regression that still leaves one or two stray bright pixels (host
    rounding, a DMA remnant, a cursor edge) technically "distinct
    colors". Pick the fraction check whose direction matches the
    region's own background/foreground polarity, and use it alongside
    ColorVarietyCheck rather than instead of it.

Deliberately not attempted here, on either check family: a specific hue or
absolute color range. A region's own painted/unpainted state and internal
variety survive a color-pipeline change (a gamma curve, a direct-color
scanout fix) that a hardcoded RGB range does not -- RegionColorCheck above
remains available for a caller with an actual reason to pin a color (a
known-fixed chrome asset), but a check written against a capture taken
while the color pipeline itself is still being corrected should assert
structure, not that capture's specific hue.

Every check class below takes (width, height, data) rather than a file
path, so it can be unit-tested against synthesized pixel data with no
screendump and no VM (see test_lib_screen.py); read_ppm() and
evaluate_screendump() are the only two functions that touch a real file.

poll_screendump_until_checks_pass() is the one exception to the no-VM
rule: a caller waiting for a set of checks to pass across several
screendumps -- the graphical-greeter boot tests' own race between a
console-text match and the guest's independently-timed X redraw --
needs a shared implementation of that wait, not a new one per test file.

DominantColorCheck is the one check here that does pin colours. It suits
a region whose correct colours are known from an independent reference
rather than read off the capture under test: it takes the region's few
most frequent colours and requires each expected colour to sit within a
tolerance of one of them. Matching the dominant colours rather than the
average keeps a dithered fill or a label's sparse ink from dragging the
answer towards a colour no pixel actually has, and it ignores a cursor
or a stray glyph that covers only a few pixels. Its optional gamma
transforms model a display whose DAC applies a gamma ramp at scanout, so
a reference measured on the far side of that ramp can still be compared.
"""

import collections
import dataclasses
import math
import time


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


def _validate_region(width, data, rect):
    """Raise ScreenCheckError unless rect (x, y, w, h) fits inside a
    width x height image, matching data's own length. Shared by every
    check below that walks a rectangle of pixels, so a rectangle that no
    longer fits (a resolution change) fails the same way -- with the same
    message -- everywhere, not just in region_average()'s own callers.
    """
    x, y, rw, rh = rect
    height = len(data) // (width * 3)
    if x < 0 or y < 0 or x + rw > width or y + rh > height:
        raise ScreenCheckError(
            f'region {rect!r} does not fit inside a {width}x{height} image')


def region_average(width, data, rect):
    """The (r, g, b) average of the rectangle (x, y, w, h) in data.

    Raises ScreenCheckError if the rectangle reaches outside the image --
    a check whose rectangle no longer fits (a resolution change) must
    fail with a clear reason, not silently average a truncated or wrong
    slice.

    Shared by RegionColorCheck and callers that need the same average
    outside a screen check.
    """
    x, y, rw, rh = rect
    _validate_region(width, data, rect)
    red = green = blue = 0
    for row in range(y, y + rh):
        offset = row * width * 3 + x * 3
        line = data[offset:offset + rw * 3]
        red += sum(line[0::3])
        green += sum(line[1::3])
        blue += sum(line[2::3])
    count = rw * rh
    return red / count, green / count, blue / count


def _region_pixels(width, data, rect):
    """Yield each pixel's (r, g, b) tuple in the rectangle (x, y, w, h),
    row-major. Shared by the two checks below that need individual
    pixels rather than region_average()'s single combined mean.
    """
    x, y, rw, rh = rect
    for row in range(y, y + rh):
        offset = row * width * 3 + x * 3
        line = data[offset:offset + rw * 3]
        for col in range(rw):
            yield (line[col * 3], line[col * 3 + 1], line[col * 3 + 2])


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
        del height  # only used by region_average via len(data)
        average = region_average(width, data, self.rect)
        in_range = all(lo <= value <= hi for value, lo, hi in
                       zip(average, self.min_rgb, self.max_rgb))
        rounded = tuple(round(value, 1) for value in average)
        if in_range:
            return True, f'{self.name} averaged {rounded}, inside range'
        return False, (
            f'{self.name} averaged {rounded}, outside the expected range '
            f'{self.min_rgb}-{self.max_rgb}')


@dataclasses.dataclass(frozen=True)
class ColorVarietyCheck:
    """Fails unless rect contains at least `min_colors` distinct (r, g, b)
    values. See this module's own docstring for why: a flat fill and a
    real bitmap can share the same region average, and only counting
    distinct colors tells them apart.
    """
    name: str
    rect: tuple
    min_colors: int

    def evaluate(self, width, height, data):
        del height
        _validate_region(width, data, self.rect)
        colors = len(set(_region_pixels(width, data, self.rect)))
        if colors >= self.min_colors:
            return True, (
                f'{self.name} has {colors} distinct colors '
                f'(>= {self.min_colors})')
        return False, (
            f'{self.name} has only {colors} distinct colors, '
            f'short of the required {self.min_colors} -- looks like a '
            'flat fill, not a rendered bitmap')


@dataclasses.dataclass(frozen=True)
class DarkPixelFractionCheck:
    """Fails unless at least `min_fraction` of rect's pixels have every
    channel at or below `max_channel`. See this module's own docstring
    for why: hard-edged glyph text is a sparse population of genuinely
    dark pixels against a light background, which a region average alone
    cannot tell apart from a uniform mid-gray fill at the same mean.
    """
    name: str
    rect: tuple
    max_channel: int
    min_fraction: float

    def evaluate(self, width, height, data):
        del height
        _validate_region(width, data, self.rect)
        pixels = list(_region_pixels(width, data, self.rect))
        dark = sum(1 for r, g, b in pixels
                  if r <= self.max_channel and g <= self.max_channel
                  and b <= self.max_channel)
        fraction = dark / len(pixels)
        rounded = round(fraction, 3)
        if fraction >= self.min_fraction:
            return True, (
                f'{self.name} is {rounded} dark (>= {self.min_fraction}, '
                f'channel <= {self.max_channel})')
        return False, (
            f'{self.name} is only {rounded} dark, short of the required '
            f'{self.min_fraction} (channel <= {self.max_channel}) -- '
            'looks unpainted, not rendered text')


@dataclasses.dataclass(frozen=True)
class LitPixelFractionCheck:
    """Fails unless at least `min_fraction` of rect's pixels have any
    channel above `min_channel` -- the opposite polarity from
    DarkPixelFractionCheck. See this module's own docstring for why: a
    dark-background render's bright foreground content (an icon fill,
    glyph strokes) is what a paint regression would erase, so a
    meaningful *fraction* of lit pixels is the direction that actually
    catches such a regression, where ColorVarietyCheck alone could still
    pass on a near-total loss that leaves only a stray bright pixel or
    two.
    """
    name: str
    rect: tuple
    min_channel: int
    min_fraction: float

    def evaluate(self, width, height, data):
        del height
        _validate_region(width, data, self.rect)
        pixels = list(_region_pixels(width, data, self.rect))
        lit = sum(1 for r, g, b in pixels
                 if r > self.min_channel or g > self.min_channel
                 or b > self.min_channel)
        fraction = lit / len(pixels)
        rounded = round(fraction, 3)
        if fraction >= self.min_fraction:
            return True, (
                f'{self.name} is {rounded} lit (>= {self.min_fraction}, '
                f'channel > {self.min_channel})')
        return False, (
            f'{self.name} is only {rounded} lit, short of the required '
            f'{self.min_fraction} (channel > {self.min_channel}) -- '
            'looks unpainted')


def gamma_encode(value, gamma):
    """Push one 0-255 channel value up through a DAC gamma-encode ramp,
    round(255 * (value / 255) ** (1 / gamma)). At gamma 1.7, 85 becomes
    134 and 170 becomes 201."""
    return round(255.0 * (value / 255.0) ** (1.0 / gamma))


def gamma_decode(value, gamma):
    """Invert gamma_encode(): round(255 * (value / 255) ** gamma), which
    maps a displayed channel value back to the framebuffer value that
    produced it, give or take one count of rounding."""
    return round(255.0 * (value / 255.0) ** gamma)


def dominant_colors(width, data, rect, top_n):
    """rect's `top_n` most frequent (r, g, b) colours, most frequent
    first, as ((r, g, b), fraction) pairs. Equal counts keep the order in
    which a row-major walk first meets them."""
    _validate_region(width, data, rect)
    counts = collections.Counter(_region_pixels(width, data, rect))
    total = rect[2] * rect[3]
    return [(color, count / total)
            for color, count in counts.most_common(top_n)]


@dataclasses.dataclass(frozen=True)
class DominantColorCheck:
    """Fails unless every colour in `expected` lies within `tolerance`
    (Euclidean RGB distance) of one of rect's `top_n` most frequent
    colours. `expected` is a tuple of (label, (r, g, b)) pairs, so one
    region can require both a background and the ink drawn over it.

    At most one of the two gamma fields may be set:

      - `encode_gamma` pushes each expected colour up through
        gamma_encode() before comparing, for a reference measured before
        the DAC ramp and a capture taken after it.
      - `decode_gamma` pushes each found colour down through
        gamma_decode() before comparing, for a reference that is itself
        a raw framebuffer value (a colour scheme entry, a real-hardware
        framebuffer dump) and a capture taken after the ramp.
    """
    name: str
    rect: tuple
    expected: tuple
    tolerance: float
    encode_gamma: float = None
    decode_gamma: float = None
    top_n: int = 6

    def __post_init__(self):
        if self.encode_gamma is not None and self.decode_gamma is not None:
            raise ValueError(
                f'{self.name}: encode_gamma and decode_gamma transform in '
                'opposite directions; set at most one')

    def evaluate(self, width, height, data):
        del height
        found = [color for color, _fraction in
                 dominant_colors(width, data, self.rect, self.top_n)]
        if self.decode_gamma is not None:
            found = [tuple(gamma_decode(channel, self.decode_gamma)
                           for channel in color) for color in found]
        passed = True
        parts = []
        for label, want in self.expected:
            if self.encode_gamma is not None:
                want = tuple(gamma_encode(channel, self.encode_gamma)
                             for channel in want)
            best = min(found, key=lambda color: math.dist(color, want))
            delta = math.dist(best, want)
            ok = delta <= self.tolerance
            passed = passed and ok
            parts.append(f'{label} want {tuple(want)} found {best} '
                         f'delta {delta:.1f}{"" if ok else " FAIL"}')
        summary = (f'{self.name} (tolerance {self.tolerance}): '
                   + ', '.join(parts))
        return passed, summary


def poll_screendump_until_checks_pass(test, vm, screendump_path, checks,
                                      interval_seconds=1.0,
                                      timeout_seconds=15.0):
    """Take a screendump every `interval_seconds`, for up to
    `timeout_seconds`, until every check in `checks` passes.

    A graphical-greeter boot test's console-text match (the serial
    getty's own login prompt) and the guest's own X server redrawing the
    graphical screen underneath it are two independent, unsynchronized
    events; a screendump taken right after the console match can still
    show the root window's unpainted stipple pattern, not yet the
    rendered dialog. The unpainted stipple pattern is itself static,
    so two consecutive equal screendumps cannot distinguish "hasn't
    redrawn yet" from "already redrawn" -- only actually reaching the
    caller's own expected state proves the redraw happened.

    Reuses evaluate_screendump() against each screendump in turn, rather
    than a second implementation of "run these checks against this
    file", so this poll's pass condition is exactly the same check
    objects (and thus the same hue-agnostic reasoning) a caller runs
    once a screendump is already known-settled.

    Returns the passing messages once every check passes, logging the
    observed settle time via test.log.info. The timeout limits when the
    last poll starts; a screendump may finish after it. Raises ScreenCheckError,
    naming the last observed per-check messages and the elapsed time, if
    the checks never all pass before the timeout.
    """
    started = time.monotonic()
    deadline = started + timeout_seconds
    passed = False
    messages = []
    attempt = 0
    while True:
        attempt += 1
        vm.cmd('screendump', filename=str(screendump_path))
        passed, messages = evaluate_screendump(screendump_path, checks)
        elapsed = time.monotonic() - started
        if passed:
            test.log.info(
                'screendump checks settled after %.2fs (%d screendump(s)): '
                '%s', elapsed, attempt, '; '.join(messages))
            return messages
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(interval_seconds, remaining))
    elapsed = time.monotonic() - started
    raise ScreenCheckError(
        f'screendump checks never all passed within {timeout_seconds}s '
        f'({elapsed:.2f}s elapsed, {attempt} screendump(s)); last result: '
        + '; '.join(messages))


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
