# Tests for the screendump-based pass conditions
#
# SPDX-License-Identifier: GPL-2.0-or-later
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

from lib_screen import (NotAllBlackCheck, RegionColorCheck, ScreenCheckError,
                        evaluate_screendump, read_ppm)


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


if __name__ == '__main__':
    unittest.main()
