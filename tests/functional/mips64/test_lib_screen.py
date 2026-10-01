# Tests for the screendump-based pass conditions
#
# SPDX-License-Identifier: BSD-3-Clause
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_lib_screen.py`, the same way
test_lib_matrix.py runs. Every check class takes (width, height, data)
directly, so these tests build small synthetic pixel buffers rather than
capturing a real screendump; read_ppm() and evaluate_screendump() are the
only two functions that touch a file, and get their own tests below using
a real temporary PPM.
"""

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from lib_screen import (ColorVarietyCheck, DarkPixelFractionCheck,
                        DominantColorCheck, LitPixelFractionCheck,
                        NotAllBlackCheck, RegionColorCheck, ScreenCheckError,
                        evaluate_screendump, gamma_decode, gamma_encode,
                        poll_screendump_until_checks_pass, read_ppm)


def solid_frame(width, height, rgb):
    """width*height pixels, every one the same (r, g, b)."""
    return bytes(rgb) * (width * height)


def write_ppm(path, width, height, data):
    with open(path, 'wb') as fobj:
        fobj.write(f'P6\n{width} {height}\n255\n'.encode('ascii'))
        fobj.write(data)


class ReadPpmTest(unittest.TestCase):

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp_path = Path(tmp.name)

    def test_reads_a_valid_ppm(self):
        path = self.tmp_path / 'frame.ppm'
        write_ppm(path, 2, 2, solid_frame(2, 2, (10, 20, 30)))

        width, height, data = read_ppm(path)

        self.assertEqual((width, height), (2, 2))
        self.assertEqual(data, bytes([10, 20, 30]) * 4)

    def test_rejects_wrong_magic(self):
        path = self.tmp_path / 'frame.ppm'
        path.write_bytes(b'P5\n1 1\n255\n\x00')

        with self.assertRaisesRegex(ScreenCheckError, 'P6'):
            read_ppm(path)

    def test_rejects_non_8bit_maxval(self):
        path = self.tmp_path / 'frame.ppm'
        path.write_bytes(b'P6\n1 1\n65535\n\x00\x00')

        with self.assertRaisesRegex(ScreenCheckError, 'maxval'):
            read_ppm(path)

    def test_rejects_truncated_pixel_data(self):
        path = self.tmp_path / 'frame.ppm'
        path.write_bytes(b'P6\n2 2\n255\n\x00\x00\x00')  # 1 of 4 pixels

        with self.assertRaisesRegex(ScreenCheckError, 'truncated'):
            read_ppm(path)


class NotAllBlackCheckTest(unittest.TestCase):

    def test_fails_on_a_fully_black_frame(self):
        # This is the RAD4 BAR2-unreachable regression's actual signature,
        # confirmed live against the pre-fix revision while developing
        # this check: every framebuffer byte stays exactly zero.
        width = height = 4
        data = solid_frame(width, height, (0, 0, 0))

        ok, message = NotAllBlackCheck().evaluate(width, height, data)

        self.assertFalse(ok)
        self.assertIn('black', message)

    def test_passes_when_any_pixel_is_above_threshold(self):
        width = height = 4
        data = bytearray(solid_frame(width, height, (0, 0, 0)))
        data[0] = 200  # one bright byte, everything else black

        ok, _message = NotAllBlackCheck().evaluate(width, height, bytes(data))

        self.assertTrue(ok)

    def test_threshold_is_exclusive(self):
        width = height = 1
        check = NotAllBlackCheck(threshold=4)

        at_threshold = solid_frame(width, height, (4, 4, 4))
        above_threshold = solid_frame(width, height, (5, 4, 4))

        self.assertFalse(check.evaluate(width, height, at_threshold)[0])
        self.assertTrue(check.evaluate(width, height, above_threshold)[0])


class RegionColorCheckTest(unittest.TestCase):

    def test_passes_when_region_average_is_in_range(self):
        width = height = 4
        data = solid_frame(width, height, (217, 217, 217))
        check = RegionColorCheck('chrome', rect=(0, 0, 2, 2),
                                 min_rgb=(190, 190, 190),
                                 max_rgb=(235, 235, 235))

        ok, message = check.evaluate(width, height, data)

        self.assertTrue(ok)
        self.assertIn('chrome', message)

    def test_fails_when_region_average_is_out_of_range(self):
        # The actual pre-fix RAD4 symptom this check is meant to catch:
        # zero red, zero green, blue capped well below a real gray.
        width = height = 4
        data = solid_frame(width, height, (0, 0, 40))
        check = RegionColorCheck('chrome', rect=(0, 0, 2, 2),
                                 min_rgb=(190, 190, 190),
                                 max_rgb=(235, 235, 235))

        ok, message = check.evaluate(width, height, data)

        self.assertFalse(ok)
        self.assertIn('outside', message)

    def test_averages_a_mixed_region_correctly(self):
        width = height = 2
        # Top-left (100,100,100), everything else (0,0,0); a 1x1 region
        # exactly on the bright pixel must average to it exactly, not be
        # diluted by neighbors outside the rectangle.
        data = bytearray(solid_frame(width, height, (0, 0, 0)))
        data[0:3] = bytes((100, 100, 100))
        check = RegionColorCheck('corner', rect=(0, 0, 1, 1),
                                 min_rgb=(100, 100, 100),
                                 max_rgb=(100, 100, 100))

        ok, _message = check.evaluate(width, height, bytes(data))

        self.assertTrue(ok)

    def test_rejects_a_region_outside_the_image(self):
        width = height = 4
        data = solid_frame(width, height, (0, 0, 0))
        check = RegionColorCheck('off-screen', rect=(2, 2, 4, 4),
                                 min_rgb=(0, 0, 0), max_rgb=(255, 255, 255))

        with self.assertRaisesRegex(ScreenCheckError, 'does not fit'):
            check.evaluate(width, height, data)


class ColorVarietyCheckTest(unittest.TestCase):

    def test_passes_when_region_has_enough_distinct_colors(self):
        # A 4x4 region where every pixel is a different shade of gray:
        # 16 distinct colors, comfortably above a min_colors of 10.
        width = height = 4
        data = bytearray(width * height * 3)
        for i in range(width * height):
            shade = i * 16
            data[i * 3:i * 3 + 3] = bytes((shade, shade, shade))
        check = ColorVarietyCheck('icon', rect=(0, 0, 4, 4), min_colors=10)

        ok, message = check.evaluate(width, height, bytes(data))

        self.assertTrue(ok)
        self.assertIn('icon', message)

    def test_fails_on_a_flat_fill(self):
        # The actual regression this check exists to catch: a real icon
        # bitmap regressing to a solid fill of its own average color.
        width = height = 4
        data = solid_frame(width, height, (100, 100, 100))
        check = ColorVarietyCheck('icon', rect=(0, 0, 4, 4), min_colors=10)

        ok, message = check.evaluate(width, height, data)

        self.assertFalse(ok)
        self.assertIn('flat fill', message)

    def test_rejects_a_region_outside_the_image(self):
        width = height = 4
        data = solid_frame(width, height, (0, 0, 0))
        check = ColorVarietyCheck('off-screen', rect=(2, 2, 4, 4),
                                  min_colors=1)

        with self.assertRaisesRegex(ScreenCheckError, 'does not fit'):
            check.evaluate(width, height, data)


class DarkPixelFractionCheckTest(unittest.TestCase):

    def test_passes_when_enough_pixels_are_dark(self):
        # Half the region black, half white: 0.5 dark, above a
        # min_fraction of 0.4.
        width, height = 4, 2
        data = bytearray(solid_frame(width, height, (255, 255, 255)))
        for row in range(height):
            offset = row * width * 3
            data[offset:offset + 2 * 3] = bytes((0, 0, 0) * 2)
        check = DarkPixelFractionCheck('label', rect=(0, 0, width, height),
                                       max_channel=10, min_fraction=0.4)

        ok, message = check.evaluate(width, height, bytes(data))

        self.assertTrue(ok)
        self.assertIn('label', message)

    def test_fails_when_too_few_pixels_are_dark(self):
        # An unpainted mid-gray region: no pixel is actually dark, so the
        # dark-pixel fraction is exactly 0, however light the average is.
        width = height = 4
        data = solid_frame(width, height, (200, 200, 200))
        check = DarkPixelFractionCheck('label', rect=(0, 0, 4, 4),
                                       max_channel=10, min_fraction=0.1)

        ok, message = check.evaluate(width, height, data)

        self.assertFalse(ok)
        self.assertIn('unpainted', message)

    def test_rejects_a_region_outside_the_image(self):
        width = height = 4
        data = solid_frame(width, height, (0, 0, 0))
        check = DarkPixelFractionCheck('off-screen', rect=(2, 2, 4, 4),
                                       max_channel=10, min_fraction=0.1)

        with self.assertRaisesRegex(ScreenCheckError, 'does not fit'):
            check.evaluate(width, height, data)


class LitPixelFractionCheckTest(unittest.TestCase):

    def test_passes_when_enough_pixels_are_lit(self):
        # Half the region black, half bright: 0.5 lit, above a
        # min_fraction of 0.4.
        width, height = 4, 2
        data = bytearray(solid_frame(width, height, (0, 0, 0)))
        for row in range(height):
            offset = row * width * 3
            data[offset:offset + 2 * 3] = bytes((200, 200, 200) * 2)
        check = LitPixelFractionCheck('icon', rect=(0, 0, width, height),
                                      min_channel=10, min_fraction=0.4)

        ok, message = check.evaluate(width, height, bytes(data))

        self.assertTrue(ok)
        self.assertIn('icon', message)

    def test_fails_when_too_few_pixels_are_lit(self):
        # A fully black region: no pixel is lit at all, however low the
        # fraction required.
        width = height = 4
        data = solid_frame(width, height, (0, 0, 0))
        check = LitPixelFractionCheck('icon', rect=(0, 0, 4, 4),
                                      min_channel=10, min_fraction=0.1)

        ok, message = check.evaluate(width, height, data)

        self.assertFalse(ok)
        self.assertIn('unpainted', message)

    def test_rejects_a_region_outside_the_image(self):
        width = height = 4
        data = solid_frame(width, height, (0, 0, 0))
        check = LitPixelFractionCheck('off-screen', rect=(2, 2, 4, 4),
                                      min_channel=10, min_fraction=0.1)

        with self.assertRaisesRegex(ScreenCheckError, 'does not fit'):
            check.evaluate(width, height, data)


class GammaTest(unittest.TestCase):

    def test_encode_matches_the_si_dac_ramp(self):
        # The two values the region set's own notes quote at gamma 1.7.
        self.assertEqual(gamma_encode(85, 1.7), 134)
        self.assertEqual(gamma_encode(170, 1.7), 201)

    def test_decode_inverts_encode_within_one_count(self):
        for value in range(256):
            with self.subTest(value=value):
                self.assertLessEqual(
                    abs(gamma_decode(gamma_encode(value, 1.7), 1.7) - value),
                    1)

    def test_ends_of_the_range_are_fixed(self):
        for convert in (gamma_encode, gamma_decode):
            self.assertEqual(convert(0, 1.7), 0)
            self.assertEqual(convert(255, 1.7), 255)


def striped_frame(width, height, colors):
    """Each row filled with colors[row % len(colors)]."""
    data = bytearray()
    for row in range(height):
        data += bytes(colors[row % len(colors)]) * width
    return bytes(data)


class DominantColorCheckTest(unittest.TestCase):

    def test_passes_when_every_expected_color_is_present(self):
        width, height = 4, 4
        data = striped_frame(width, height, ((200, 200, 200), (0, 0, 0)))
        check = DominantColorCheck(
            'button', (0, 0, 4, 4),
            (('body', (195, 195, 195)), ('ink', (0, 0, 0))), tolerance=10)

        ok, message = check.evaluate(width, height, data)

        self.assertTrue(ok, message)
        self.assertIn('button', message)

    def test_fails_when_one_expected_color_is_missing(self):
        # The body matches but no ink was drawn.
        width = height = 4
        data = solid_frame(width, height, (200, 200, 200))
        check = DominantColorCheck(
            'button', (0, 0, 4, 4),
            (('body', (200, 200, 200)), ('ink', (0, 0, 0))), tolerance=10)

        ok, message = check.evaluate(width, height, data)

        self.assertFalse(ok)
        self.assertIn('ink want (0, 0, 0)', message)
        self.assertIn('FAIL', message)

    def test_tolerance_is_inclusive(self):
        width = height = 2
        data = solid_frame(width, height, (0, 0, 10))
        at = DominantColorCheck('fill', (0, 0, 2, 2),
                                (('fill', (0, 0, 0)),), tolerance=10)
        under = DominantColorCheck('fill', (0, 0, 2, 2),
                                   (('fill', (0, 0, 0)),), tolerance=9.9)

        self.assertTrue(at.evaluate(width, height, data)[0])
        self.assertFalse(under.evaluate(width, height, data)[0])

    def test_ignores_colors_outside_the_most_frequent(self):
        # Seven colours; the rarest (one pixel) matches but is not among
        # the top six, so the check must not find it.
        width, height = 13, 1
        colors = [(10 * i, 0, 0) for i in range(6) for _ in range(2)]
        colors.append((0, 255, 0))
        data = b''.join(bytes(color) for color in colors)
        check = DominantColorCheck('strip', (0, 0, 13, 1),
                                   (('green', (0, 255, 0)),), tolerance=1)

        self.assertFalse(check.evaluate(width, height, data)[0])

    def test_encode_gamma_raises_the_expected_color(self):
        # A pre-ramp reference of 85 grey compared against a post-ramp
        # capture showing 134 grey.
        width = height = 2
        data = solid_frame(width, height, (134, 134, 134))
        plain = DominantColorCheck('mat', (0, 0, 2, 2),
                                   (('dark', (85, 85, 85)),), tolerance=2)
        encoded = DominantColorCheck('mat', (0, 0, 2, 2),
                                     (('dark', (85, 85, 85)),), tolerance=2,
                                     encode_gamma=1.7)

        self.assertFalse(plain.evaluate(width, height, data)[0])
        self.assertTrue(encoded.evaluate(width, height, data)[0])

    def test_decode_gamma_lowers_the_found_color(self):
        width = height = 2
        data = solid_frame(width, height, (134, 134, 134))
        decoded = DominantColorCheck('backdrop', (0, 0, 2, 2),
                                     (('tone', (85, 85, 85)),), tolerance=2,
                                     decode_gamma=1.7)

        self.assertTrue(decoded.evaluate(width, height, data)[0])

    def test_rejects_both_gamma_directions_at_once(self):
        with self.assertRaisesRegex(ValueError, 'at most one'):
            DominantColorCheck('both', (0, 0, 1, 1), (('x', (0, 0, 0)),),
                               tolerance=1, encode_gamma=1.7,
                               decode_gamma=1.7)

    def test_rejects_a_region_outside_the_image(self):
        width = height = 4
        data = solid_frame(width, height, (0, 0, 0))
        check = DominantColorCheck('off-screen', (2, 2, 4, 4),
                                   (('x', (0, 0, 0)),), tolerance=1)

        with self.assertRaisesRegex(ScreenCheckError, 'does not fit'):
            check.evaluate(width, height, data)


class EvaluateScreendumpTest(unittest.TestCase):

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp_path = Path(tmp.name)

    def test_runs_every_check_and_reports_each(self):
        path = self.tmp_path / 'frame.ppm'
        write_ppm(path, 2, 2, solid_frame(2, 2, (217, 217, 217)))

        passed, messages = evaluate_screendump(path, [
            NotAllBlackCheck(),
            RegionColorCheck('chrome', rect=(0, 0, 2, 2),
                             min_rgb=(190, 190, 190),
                             max_rgb=(235, 235, 235)),
        ])

        self.assertTrue(passed)
        self.assertEqual(len(messages), 2)

    def test_one_failing_check_fails_the_whole_result(self):
        path = self.tmp_path / 'frame.ppm'
        write_ppm(path, 2, 2, solid_frame(2, 2, (0, 0, 0)))

        passed, messages = evaluate_screendump(path, [
            NotAllBlackCheck(),
            RegionColorCheck('chrome', rect=(0, 0, 2, 2),
                             min_rgb=(190, 190, 190),
                             max_rgb=(235, 235, 235)),
        ])

        self.assertFalse(passed)
        self.assertEqual(len(messages), 2)  # both still ran and reported


class _FakeLog:
    """Swallows the .info() calls poll_screendump_until_checks_pass()
    makes on a real qemu_test logger."""

    def info(self, *args, **kwargs):
        pass


class _FakeTest:
    """Just enough of a QemuSystemTest for
    poll_screendump_until_checks_pass()'s own test.log.info() call."""

    def __init__(self):
        self.log = _FakeLog()


class _FakeVm:
    """Emits one queued (width, height, data) frame per `screendump`
    command, writing it to the requested path -- enough to drive
    poll_screendump_until_checks_pass() without a real VM or QMP
    connection."""

    def __init__(self, frames):
        self._frames = list(frames)

    def cmd(self, command, filename):
        assert command == 'screendump'
        width, height, data = self._frames.pop(0)
        write_ppm(filename, width, height, data)


class PollScreendumpUntilChecksPassTest(unittest.TestCase):

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp_path = Path(tmp.name)

    def test_returns_once_every_check_passes(self):
        # First screendump is black (NotAllBlackCheck still fails);
        # the poll must take a second dump rather than judging the
        # first one twice.
        width = height = 2
        black = solid_frame(width, height, (0, 0, 0))
        lit = solid_frame(width, height, (200, 200, 200))
        vm = _FakeVm([(width, height, black), (width, height, lit)])
        path = self.tmp_path / 'poll.ppm'

        messages = poll_screendump_until_checks_pass(
            _FakeTest(), vm, path, [NotAllBlackCheck()],
            interval_seconds=0.01, timeout_seconds=0.05)

        self.assertEqual(len(messages), 1)

    def test_uses_elapsed_deadline_instead_of_dump_count(self):
        width = height = 2
        black = solid_frame(width, height, (0, 0, 0))
        lit = solid_frame(width, height, (200, 200, 200))
        vm = _FakeVm([(width, height, black)] * 3 +
                     [(width, height, lit)])
        now = [0.0]

        def advance(seconds):
            now[0] += seconds

        with patch('lib_screen.time.monotonic', side_effect=lambda: now[0]), \
             patch('lib_screen.time.sleep', side_effect=advance):
            messages = poll_screendump_until_checks_pass(
                _FakeTest(), vm, self.tmp_path / 'poll.ppm',
                [NotAllBlackCheck()], interval_seconds=1.0,
                timeout_seconds=3.5)

        self.assertEqual(len(messages), 1)
        self.assertEqual(now[0], 3.0)

    def test_raises_if_the_checks_never_all_pass(self):
        width = height = 2
        black = solid_frame(width, height, (0, 0, 0))
        vm = _FakeVm([(width, height, black)] * 5)
        path = self.tmp_path / 'poll.ppm'

        with self.assertRaisesRegex(ScreenCheckError, 'never all passed'):
            poll_screendump_until_checks_pass(
                _FakeTest(), vm, path, [NotAllBlackCheck()],
                interval_seconds=0.01, timeout_seconds=0.03)


if __name__ == '__main__':
    unittest.main()
