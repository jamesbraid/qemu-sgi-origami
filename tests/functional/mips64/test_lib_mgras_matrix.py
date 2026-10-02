# Tests for the MGRAS per-board matrix helpers
#
# SPDX-License-Identifier: BSD-3-Clause
"""
A plain unittest with no QEMU: it checks which features run on which
board, the -device spelling, and that the colour-statistics comparison
accepts a frame like its reference and rejects a different one.
"""

import os
import tempfile
import unittest

import lib_mgras_matrix as m

WIDTH, HEIGHT = 64, 48


def frame(colors):
    """A frame striped across its rows in the given colours."""
    rows = []
    stripe = HEIGHT // len(colors)
    for y in range(HEIGHT):
        color = colors[min(y // stripe, len(colors) - 1)]
        rows.append(bytes(color) * WIDTH)
    return bytes().join(rows)


class MatrixTables(unittest.TestCase):
    def test_features_per_board(self):
        names = {key: [f.name for f in m.features_for(board)]
                 for key, board in m.BOARDS.items()}
        self.assertEqual(names['si'], ['lit3d', 'pixels'])
        self.assertEqual(names['si-t'], ['lit3d', 'texture', 'pixels'])
        self.assertEqual(names['ssi'], ['lit3d', 'pixels', 'doublebuffer'])
        self.assertEqual(names['essi'], ['lit3d', 'pixels', 'doublebuffer'])

    def test_refused_boards_absent(self):
        self.assertNotIn('mxi', m.BOARDS)
        self.assertNotIn('emxi', m.BOARDS)

    def test_device_args(self):
        self.assertEqual(m.device_args(m.BOARDS['si']),
                         ('-device', 'sgi-mgras,slot=io3,board=si'))
        self.assertIn('x-unimplemented-board=on',
                      m.device_args(m.BOARDS['si-t'])[1])


class Statistics(unittest.TestCase):
    def test_round_trip_and_mismatch(self):
        red_blue = frame([(200, 0, 0), (0, 0, 200)])
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'ref.json')
            m.write_stats(path, WIDTH, red_blue)
            ref = m.load_stats(path)
        self.assertTrue(m.compare_stats(ref, WIDTH, red_blue)[0])
        # A slightly different shade is still inside the tolerance.
        near = frame([(190, 5, 0), (0, 5, 190)])
        self.assertTrue(m.compare_stats(ref, WIDTH, near)[0])
        green = frame([(0, 200, 0), (0, 200, 0)])
        self.assertFalse(m.compare_stats(ref, WIDTH, green)[0])

    def test_changed_fraction(self):
        black = (WIDTH, HEIGHT, frame([(0, 0, 0)]))
        half = (WIDTH, HEIGHT, frame([(0, 0, 0), (200, 200, 200)]))
        self.assertEqual(m.changed_fraction(black, black), 0.0)
        self.assertAlmostEqual(m.changed_fraction(black, half), 0.5)


if __name__ == '__main__':
    unittest.main()
