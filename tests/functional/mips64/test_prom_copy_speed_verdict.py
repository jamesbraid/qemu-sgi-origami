#!/usr/bin/env python3
#
# Equivalence test: test_prom_copy_speed's copy-bracket verdict, without
# booting anything.
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, runnable with
`uv run --with pytest pytest tests/functional/mips64/test_prom_copy_speed_verdict.py -v`.
It exercises copy_bracket_seconds(), copy_is_fast() and record_first_seen(),
the pure functions test_prom_copy_speed.py factors its copy-speed check into.

Proves the verdict without a boot: a copy loop within budget passes, a
minutes-long one (the I/O-mirror regression) fails and names the duration, a
bracket that never closed is not "fast", and first-seen stamps the earliest
sighting of each bracket code.
"""

import unittest

from test_prom_copy_speed import (
    COPY_BRACKET_START,
    COPY_BRACKET_END,
    COPY_BRACKET_MAX_SECONDS,
    copy_bracket_seconds,
    copy_is_fast,
    record_first_seen,
)


def _trace_line(code, kind='progress', cpu=0, meaning='fixture'):
    """One sgi_hub_led trace line, in the exact shape QEMU emits it."""
    return f'sgi_hub_led CPU {cpu}: {code} ({kind}: {meaning})'


class CopyBracketVerdictTest(unittest.TestCase):

    def test_fixed_copy_is_fast(self):
        # A few seconds apart, as the romd view of the flash produces.
        ok, duration = copy_is_fast({COPY_BRACKET_START: 100.0,
                                     COPY_BRACKET_END: 103.0})
        self.assertTrue(ok)
        self.assertAlmostEqual(duration, 3.0)

    def test_io_mirror_copy_fails(self):
        # The ~223s copy the I/O mirror produces: over budget, and the
        # duration is reported so the failure names the real cost.
        ok, duration = copy_is_fast({COPY_BRACKET_START: 2.0,
                                     COPY_BRACKET_END: 225.0})
        self.assertFalse(ok)
        self.assertAlmostEqual(duration, 223.0)

    def test_exactly_at_budget_passes(self):
        ok, _ = copy_is_fast({COPY_BRACKET_START: 0.0,
                              COPY_BRACKET_END: COPY_BRACKET_MAX_SECONDS})
        self.assertTrue(ok)

    def test_bracket_that_never_closed_is_not_fast(self):
        ok, duration = copy_is_fast({COPY_BRACKET_START: 5.0})
        self.assertFalse(ok)
        self.assertIsNone(duration)

    def test_no_codes_is_not_fast(self):
        ok, duration = copy_is_fast({})
        self.assertFalse(ok)
        self.assertIsNone(duration)

    def test_record_first_seen_keeps_earliest(self):
        first_seen = {}
        # 0x5c appears first, alone.
        record_first_seen(first_seen, _trace_line(COPY_BRACKET_START), 10.0)
        # Later poll: 0x5c still present (must keep 10.0) and 0x3c now present.
        record_first_seen(
            first_seen,
            '\n'.join([_trace_line(COPY_BRACKET_START),
                       _trace_line(COPY_BRACKET_END)]),
            13.0)
        self.assertEqual(first_seen[COPY_BRACKET_START], 10.0)
        self.assertEqual(first_seen[COPY_BRACKET_END], 13.0)
        self.assertAlmostEqual(copy_bracket_seconds(first_seen), 3.0)

    def test_record_first_seen_ignores_other_codes(self):
        first_seen = {}
        record_first_seen(first_seen, _trace_line('0x40'), 7.0)
        self.assertEqual(first_seen, {})


if __name__ == '__main__':
    unittest.main()
