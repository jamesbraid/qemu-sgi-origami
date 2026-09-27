# Tests for the SI greeter and desktop region colour tables
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_lib_mgras_regions.py`. It paints a
synthetic 1280x1024 frame the way SI's DAC would show each region's
expected colours, and checks that every region passes on it and fails on
a black frame, so a typo in a box or a gamma direction shows up here
rather than as a mysterious boot-test failure.
"""

import unittest

from lib_mgras_regions import (DESKTOP_COLOR_CHECKS, GREETER_COLOR_CHECKS,
                               SI_DAC_GAMMA)
from lib_screen import gamma_encode

WIDTH, HEIGHT = 1280, 1024


def displayed(check, rgb):
    """The post-DAC colour SI would show for an expected colour of this
    check. A decode_gamma check's reference is a raw framebuffer value,
    so the DAC ramp still has to be applied to it; an encode_gamma check
    applies the ramp to its own reference."""
    gamma = check.encode_gamma or check.decode_gamma
    if gamma is None:
        return tuple(rgb)
    return tuple(gamma_encode(channel, gamma) for channel in rgb)


def paint(checks):
    """A frame with each check's rect striped, one row per expected
    colour in turn, in the colours SI would display."""
    frame = bytearray(WIDTH * HEIGHT * 3)
    for check in checks:
        x, y, w, h = check.rect
        colors = [displayed(check, rgb) for _label, rgb in check.expected]
        for row in range(y, y + h):
            color = colors[(row - y) % len(colors)]
            offset = (row * WIDTH + x) * 3
            frame[offset:offset + w * 3] = bytes(color) * w
    return bytes(frame)


class RegionTableTest(unittest.TestCase):

    def test_every_region_passes_on_its_displayed_colors(self):
        for checks in (GREETER_COLOR_CHECKS, DESKTOP_COLOR_CHECKS):
            frame = paint(checks)
            for check in checks:
                with self.subTest(region=check.name):
                    ok, message = check.evaluate(WIDTH, HEIGHT, frame)
                    self.assertTrue(ok, message)

    def test_every_region_fails_on_a_black_frame(self):
        frame = bytes(WIDTH * HEIGHT * 3)
        for check in GREETER_COLOR_CHECKS + DESKTOP_COLOR_CHECKS:
            with self.subTest(region=check.name):
                ok, _message = check.evaluate(WIDTH, HEIGHT, frame)
                self.assertFalse(ok)

    def test_region_names_are_unique(self):
        names = [check.name for check in
                 GREETER_COLOR_CHECKS + DESKTOP_COLOR_CHECKS]
        self.assertEqual(len(names), len(set(names)))

    def test_greeter_has_ten_regions(self):
        # The count of scored greeter regions in the source region set;
        # a region silently dropped in a later copy should show here.
        self.assertEqual(len(GREETER_COLOR_CHECKS), 10)

    def test_gamma_is_the_si_dac_fit(self):
        for check in GREETER_COLOR_CHECKS + DESKTOP_COLOR_CHECKS:
            for gamma in (check.encode_gamma, check.decode_gamma):
                if gamma is not None:
                    self.assertEqual(gamma, SI_DAC_GAMMA)


if __name__ == '__main__':
    unittest.main()
